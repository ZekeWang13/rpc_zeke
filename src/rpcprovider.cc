#include "rpcprovider.h"
#include "mprpcapplication.h"
#include "rpcheader.pb.h"
#include "logger.h"
#include "zookeeperutil.h"
#include <cstring>
#include <limits>

/*
service_name =>  service描述   
                        =》 service* 记录服务对象
                        method_name  =>  method方法对象
*/
// 提供给外部使用的，可以发布rpc方法的函数
void RpcProvider::NotifyService(google::protobuf::Service *service)
{
    ServiceInfo service_info;

    // 获取了服务对象的描述信息
    const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();
    // 获取服务的名字
    std::string service_name = pserviceDesc->name();
    // 获取服务对象service的方法的数量
    // 方法是有下标的，在后续的CallMethod内部其实也用的是下标
    int methodCnt = pserviceDesc->method_count();

    // std::cout << "service_name:" << service_name << std::endl;
    LOG_INFO("service_name:%s", service_name.c_str());

    for (int i=0; i < methodCnt; ++i)
    {
        // 获取了服务对象指定下标的服务方法的描述（抽象描述） UserServiceRPC   Login
        const google::protobuf::MethodDescriptor* pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        service_info.m_methodMap.insert({method_name, pmethodDesc});

        LOG_INFO("method_name:%s", method_name.c_str());
    }
    service_info.m_service = service;
    m_serviceMap.insert({service_name, service_info});
}

// 启动rpc服务节点，开始提供rpc远程网络调用服务
void RpcProvider::Run()
{
    // 读取配置文件rpcserver的信息
    std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());
    m_long_connection = MprpcApplication::GetInstance().GetConfig().Load("rpcserverlongconnection") == "true";
    muduo::net::InetAddress address(ip, port);

    // 创建TcpServer对象
    muduo::net::TcpServer server(&m_eventLoop, address, "RpcProvider");

    // 绑定连接回调和消息读写回调方法
    server.setConnectionCallback(std::bind(&RpcProvider::OnConnection, this, std::placeholders::_1));
    // 等价于：
    // [ this ](auto&& arg1) {
    //     return this->OnConnection(arg1);
    // }
    server.setMessageCallback(std::bind(&RpcProvider::OnMessage, this, std::placeholders::_1, 
            std::placeholders::_2, std::placeholders::_3));

    // 设置muduo库的线程数量
    server.setThreadNum(4);

    // 把当前rpc节点上要发布的服务全部注册到zk上面，让rpc client可以从zk上发现服务
    // session timeout   30s     zkclient 网络I/O线程  1/3 * timeout 时间发送ping消息
    ZkClient zkCli;
    zkCli.Start();
    //负载均衡
    // service_name 为永久性节点
    // method_name 为永久性节点
    // providerxxxx 为临时顺序节点，节点值存 ip:port
    for (auto &sp : m_serviceMap)
    {
        // /service_name   /UserServiceRpc
        std::string service_path = "/" + sp.first;
        zkCli.Create(service_path.c_str(), nullptr, 0);

        for (auto &mp : sp.second.m_methodMap)
        {
            // /service_name/method_name   /UserServiceRpc/Login
            std::string method_path = service_path + "/" + mp.first;
            zkCli.Create(method_path.c_str(), nullptr, 0);

            // 在 method_path 下面创建临时顺序子节点
            // /UserServiceRpc/Login/provider000000000x
            char method_path_data[128] = {0};
            sprintf(method_path_data, "%s:%d", ip.c_str(), port);

            std::string instance_path = method_path + "/provider";
            zkCli.Create(instance_path.c_str(),
                        method_path_data,
                        strlen(method_path_data),
                        ZOO_EPHEMERAL | ZOO_SEQUENCE);
        }
    }
    // // service_name为永久性节点    method_name为临时性节点
    // for (auto &sp : m_serviceMap) 
    // {
    //     // /service_name   /UserServiceRpc
    //     std::string service_path = "/" + sp.first;
    //     zkCli.Create(service_path.c_str(), nullptr, 0);
    //     for (auto &mp : sp.second.m_methodMap)
    //     {
    //         // /service_name/method_name   /UserServiceRpc/Login 存储当前这个rpc服务节点主机的ip和port
    //         std::string method_path = service_path + "/" + mp.first;
    //         char method_path_data[128] = {0};
    //         sprintf(method_path_data, "%s:%d", ip.c_str(), port);
    //         // ZOO_EPHEMERA临时性节点
    //         zkCli.Create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
    //     }
    // }

    // rpc服务端准备启动，打印信息
    std::cout << "RpcProvider start service at ip:" << ip << " port:" << port << std::endl;

    // 启动网络服务
    server.start();
    //相当于epollwait
    m_eventLoop.loop(); 
}

// 新的socket连接回调
void RpcProvider::OnConnection(const muduo::net::TcpConnectionPtr &conn)
{
    if (!conn->connected())
    {
        // 和rpc client的连接断开了
        conn->shutdown();
    }
}

/*
RpcProvider和RpcConsumer协商好之间通信用的protobufxieyi
service_name method_name args    
*/
// 如果远程有一个rpc服务的调用请求，那么OnMessage方法就会响应
// buffer存放的是读到的数据
void RpcProvider::OnMessage(const muduo::net::TcpConnectionPtr &conn, 
                            muduo::net::Buffer *buffer, 
                            muduo::Timestamp)
{
    // TCP 是字节流；缓冲区不足一个完整帧时保留数据，等待下次回调。
    if (buffer->readableBytes() < sizeof(uint32_t))
    {
        return;
    }

    uint32_t header_size = 0;
    memcpy(&header_size, buffer->peek(), sizeof(header_size));
    if (header_size > 1024 * 1024)
    {
        LOG_ERR("rpc header is too large: %u", header_size);
        conn->shutdown();
        return;
    }
    if (buffer->readableBytes() < sizeof(uint32_t) + header_size)
    {
        return;
    }

    // 根据header_size读取数据头的原始字符流，反序列化数据，得到rpc请求的详细信息
    std::string rpc_header_str(buffer->peek() + sizeof(uint32_t), header_size);
    mprpc::RpcHeader rpcHeader;
    std::string service_name;
    std::string method_name;
    uint32_t args_size;
    if (rpcHeader.ParseFromString(rpc_header_str))
    {
        // 数据头反序列化成功
        service_name = rpcHeader.service_name();
        method_name = rpcHeader.method_name();
        args_size = rpcHeader.args_size();
    }
    else
    {
        // 数据头反序列化失败
        std::cout << "rpc_header_str:" << rpc_header_str << " parse error!" << std::endl;
        conn->shutdown();
        return;
    }

    const size_t frame_size = sizeof(uint32_t) + header_size + args_size;
    if (frame_size < header_size || frame_size > 64 * 1024 * 1024)
    {
        LOG_ERR("rpc request frame is too large");
        conn->shutdown();
        return;
    }
    if (buffer->readableBytes() < frame_size)
    {
        return;
    }
    std::string recv_buf = buffer->retrieveAsString(frame_size);

    // 获取rpc方法参数的字符流数据
    std::string args_str = recv_buf.substr(4 + header_size, args_size);

    // 逐请求跟踪默认关闭，避免控制台 I/O 成为吞吐瓶颈。
#ifdef MPRPC_ENABLE_REQUEST_TRACE
    std::cout << "============================================" << std::endl;
    std::cout << "header_size: " << header_size << std::endl; 
    std::cout << "rpc_header_str: " << rpc_header_str << std::endl; 
    std::cout << "service_name: " << service_name << std::endl; 
    std::cout << "method_name: " << method_name << std::endl; 
    std::cout << "args_str: " << args_str << std::endl; 
    std::cout << "============================================" << std::endl;
#endif

    // 获取service对象和method对象
    auto it = m_serviceMap.find(service_name);
    if (it == m_serviceMap.end())
    {
        std::cout << service_name << " is not exist!" << std::endl;
        return;
    }

    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end())
    {
        std::cout << service_name << ":" << method_name << " is not exist!" << std::endl;
        return;
    }

    google::protobuf::Service *service = it->second.m_service; // 获取service对象  new UserService
    const google::protobuf::MethodDescriptor *method = mit->second; // 获取method对象描述指针  Login

    // 生成rpc方法调用的请求request和响应response参数
    google::protobuf::Message *request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str))
    {
        std::cout << "request parse error, content:" << args_str << std::endl;
        return;
    }
    google::protobuf::Message *response = service->GetResponsePrototype(method).New();

    // 给下面的method方法的调用，绑定一个Closure的回调函数给RUN
    // 为什么要done？因为要解耦操作，业务层只需要run就行了，不需要发送数据。
    google::protobuf::Closure *done = google::protobuf::NewCallback<RpcProvider, 
                                                const muduo::net::TcpConnectionPtr&, 
                                                google::protobuf::Message*>
                                                (this, &RpcProvider::SendRpcResponse, 
                                                                    conn, response);

    // 在框架上根据远端rpc请求，调用当前rpc节点上发布的方法
    // new UserService().Login(controller, request, response, done)
    service->CallMethod(method, nullptr, request, response, done);
}

// Closure的回调操作，用于序列化rpc的响应和网络发送
void RpcProvider::SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, google::protobuf::Message *response)
{
    std::string response_str;
    if (response->SerializeToString(&response_str)) // response进行序列化
    {
        if (response_str.size() > std::numeric_limits<uint32_t>::max())
        {
            LOG_ERR("rpc response is too large");
            conn->shutdown();
            return;
        }

        // 响应帧格式：response_size(4 bytes) + protobuf response。
        uint32_t response_size = static_cast<uint32_t>(response_str.size());
        std::string framed_response(reinterpret_cast<const char*>(&response_size),
                                    sizeof(response_size));
        framed_response += response_str;
        conn->send(framed_response);
    }
    else
    {
        std::cout << "serialize response_str error!" << std::endl; 
    }
    // 默认保持原短连接行为；开启后由客户端复用并在析构时关闭连接。
    if (!m_long_connection)
    {
        conn->shutdown();
    }
}
