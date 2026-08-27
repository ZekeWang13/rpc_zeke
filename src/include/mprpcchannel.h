#pragma once

#include <google/protobuf/service.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
//负载均衡
#include <vector>
#include <atomic>
#include <mutex>
#include <string>

class MprpcChannel : public google::protobuf::RpcChannel
{
public:
    MprpcChannel();
    ~MprpcChannel() override;
    // 所有通过stub代理对象调用的rpc方法，都走到这里了，统一做rpc方法调用的数据数据序列化和网络发送 
    void CallMethod(const google::protobuf::MethodDescriptor* method,
                          google::protobuf::RpcController* controller, 
                          const google::protobuf::Message* request,
                          google::protobuf::Message* response,
                          google::protobuf:: Closure* done);

private:
    void CloseConnection(int fd);

    // 长连接模式下，一个 Channel 同一时刻只处理一个请求。
    int m_clientfd;
    std::string m_provider_address;
    std::mutex m_connection_mutex;
};
