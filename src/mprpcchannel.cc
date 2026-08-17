#include "mprpcchannel.h"
#include <string>
#include "rpcheader.pb.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include "mprpcapplication.h"
#include "mprpccontroller.h"
#include "zookeeperutil.h"
#include "circuitbreaker.h"
#include "logger.h"
#include "servicediscovery.h"

/*
header_size + service_name method_name args_size + args
*/
// 所有通过stub代理对象调用的rpc方法，都走到这里了，统一做rpc方法调用的数据数据序列化和网络发送和数据收取
void MprpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                                google::protobuf::RpcController* controller, 
                                const google::protobuf::Message* request,
                                google::protobuf::Message* response,
                                google::protobuf:: Closure* done)
{
    const google::protobuf::ServiceDescriptor* sd = method->service();
    std::string service_name = sd->name(); // service_name
    std::string method_name = method->name(); // method_name

    //熔断机制
    auto breaker = CircuitBreakerManager::GetInstance().GetBreaker(service_name, method_name);
    if (!breaker->AllowRequest())
    {
        controller->SetFailed("circuit breaker is open, request rejected");
        LOG_ERR("rpc call rejected by circuit breaker, service:%s method:%s",
                service_name.c_str(), method_name.c_str());
        return;
    }

    // 获取参数的序列化字符串长度
    uint32_t args_size = 0;
    std::string args_str;
    if (request->SerializeToString(&args_str))
    {
        args_size = args_str.size();
    }
    else
    {
        controller->SetFailed("serialize request error!");
        return;
    }
    
    // 定义rpc的请求header
    mprpc::RpcHeader rpcHeader;
    rpcHeader.set_service_name(service_name);
    rpcHeader.set_method_name(method_name);
    rpcHeader.set_args_size(args_size);

    uint32_t header_size = 0;
    std::string rpc_header_str;
    if (rpcHeader.SerializeToString(&rpc_header_str))
    {
        header_size = rpc_header_str.size();
    }
    else
    {
        controller->SetFailed("serialize rpc header error!");
        return;
    }

    // 组织待发送的rpc请求的字符串
    std::string send_rpc_str;
    send_rpc_str.insert(0, std::string((char*)&header_size, 4)); // header_size
    send_rpc_str += rpc_header_str; // rpcheader
    send_rpc_str += args_str; // args

    // 打印调试信息
    std::cout << "============================================" << std::endl;
    std::cout << "header_size: " << header_size << std::endl; 
    std::cout << "rpc_header_str: " << rpc_header_str << std::endl; 
    std::cout << "service_name: " << service_name << std::endl; 
    std::cout << "method_name: " << method_name << std::endl; 
    std::cout << "args_str: " << args_str << std::endl; 
    std::cout << "============================================" << std::endl;

    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd)
    {
        char errtxt[512] = {0};
        sprintf(errtxt, "create socket error! errno:%d", errno);
        controller->SetFailed(errtxt);
        return;
    }

    // 读取配置文件rpcserver的信息
    // std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    // uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());
    // rpc调用方想调用service_name的method_name服务，需要查询zk上该服务所在的host信息
    

    // 直接给单例去做，不再每次都进行连接————————————————————————————————
    // ZkClient zkCli;
    // zkCli.Start();
    // //负载均衡
    // // /UserServiceRpc/Login
    // std::string method_path = "/" + service_name + "/" + method_name;

    // // 获取该方法下的所有 provider 子节点
    // std::vector<std::string> providers = zkCli.GetChildren(method_path.c_str());
    // if (providers.empty())
    // {
    //     close(clientfd);
    //     controller->SetFailed(method_path + " has no provider!");
    //     return;
    // }

    // // 排序调试
    // std::sort(providers.begin(), providers.end());

    // std::cout << "providers: ";
    // for (const auto &p : providers)
    // {
    //     std::cout << p << " ";
    // }
    // std::cout << std::endl;

    // //随机
    // static bool seeded = false;
    // if (!seeded)
    // {
    //     srand(time(nullptr) ^ getpid());
    //     seeded = true;
    // }

    // std::string selected_provider = providers[rand() % providers.size()];
    // std::cout << "selected_provider: " << selected_provider << std::endl;

    // // /OrderServiceRPC/check/provider0000000002
    // std::string provider_path = method_path + "/" + selected_provider;

    // // 获取 provider 对应的 host_data，例如 127.0.0.1:8000
    // std::string host_data = zkCli.GetData(provider_path.c_str());
    // if (host_data.empty())
    // {
    //     close(clientfd);
    //     controller->SetFailed(provider_path + " data is empty!");
    //     return;
    // }

    // std::cout << "host_data: [" << host_data << "]" << std::endl;

    // size_t idx = host_data.find(":");
    // if (idx == std::string::npos)
    // {
    //     close(clientfd);
    //     controller->SetFailed(provider_path + " address is invalid!");
    //     return;
    // }

    // std::string ip = host_data.substr(0, idx);
    // uint16_t port = atoi(host_data.substr(idx + 1).c_str());

    // std::cout << "ip: [" << ip << "], port: [" << port << "]" << std::endl;

    // //  /UserServiceRpc/Login
    // std::string method_path = "/" + service_name + "/" + method_name;
    // // 127.0.0.1:8000
    // std::string host_data = zkCli.GetData(method_path.c_str());
    // if (host_data == "")
    // {
    //     controller->SetFailed(method_path + " is not exist!");
    //     return;
    // }
    // int idx = host_data.find(":");
    // if (idx == -1)
    // {
    //     controller->SetFailed(method_path + " address is invalid!");
    //     return;
    // }
    // std::string ip = host_data.substr(0, idx);
    // uint16_t port = atoi(host_data.substr(idx+1, host_data.size()-idx).c_str()); 
    // 直接给单例去做，不再每次都进行连接————————————————————————————————
    ProviderInfo provider;
    if (!ServiceDiscovery::GetInstance().GetProvider(service_name, method_name, provider))
    {
        close(clientfd);
        controller->SetFailed("no available provider from local cache/zookeeper");
        breaker->OnFailure();
        return;
    }

    std::string host_data = provider.host_data;
    size_t idx = host_data.find(":");
    if (idx == std::string::npos)
    {
        close(clientfd);
        controller->SetFailed("provider address is invalid: " + host_data);
        breaker->OnFailure();
        return;
    }

    std::string ip = host_data.substr(0, idx);
    uint16_t port = atoi(host_data.substr(idx + 1).c_str());
    std::cout << "ip: [" << ip << "], port: [" << port << "]" << std::endl;

    LOG_INFO("selected provider node:%s addr:%s",
            provider.node_name.c_str(), provider.host_data.c_str());

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip.c_str());

    // 连接rpc服务节点
    if (-1 == connect(clientfd, (struct sockaddr*)&server_addr, sizeof(server_addr)))
    {
        close(clientfd);
        char errtxt[512] = {0};
        sprintf(errtxt, "connect error! errno:%d", errno);
        controller->SetFailed(errtxt);

        //熔断机制
        breaker->OnFailure();
        LOG_ERR("rpc connect failed, service:%s method:%s errno:%d",
            service_name.c_str(), method_name.c_str(), errno);
        return;
    }
    
    //超时控制
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;

    setsockopt(clientfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(clientfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // 发送rpc请求
    if (-1 == send(clientfd, send_rpc_str.c_str(), send_rpc_str.size(), 0))
    {
        close(clientfd);
        char errtxt[512] = {0};
        sprintf(errtxt, "send error! errno:%d", errno);
        controller->SetFailed(errtxt);
        breaker->OnFailure();
        return;
    }

    // 接收rpc请求的响应值
    char recv_buf[1024] = {0};
    int recv_size = 0;
    if (-1 == (recv_size = recv(clientfd, recv_buf, 1024, 0)))
    {
        close(clientfd);
        char errtxt[512] = {0};
        sprintf(errtxt, "recv error! errno:%d", errno);
        controller->SetFailed(errtxt);
        breaker->OnFailure();
        return;
    }

    // 反序列化rpc调用的响应数据
    if (!response->ParseFromArray(recv_buf, recv_size))
    {
        close(clientfd);
        char errtxt[512] = {0};
        sprintf(errtxt, "parse error! response_str:%s", recv_buf);
        controller->SetFailed(errtxt);
        breaker->OnFailure();
        return;
    }

    breaker->OnSuccess();
    close(clientfd);
}