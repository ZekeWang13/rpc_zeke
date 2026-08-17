#pragma once

#include "zookeeperutil.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <memory>

struct ProviderInfo
{
    std::string node_name;   // provider0000000001
    std::string host_data;   // 127.0.0.1:8001
};

class ServiceDiscovery
{
public:
    static ServiceDiscovery& GetInstance();

    // 启动服务发现模块，建立长连接
    void Start();

    // 初始化某个 service.method 的缓存和 watch
    void WatchMethod(const std::string& service_name, const std::string& method_name);

    // 获取一个 provider（供 MprpcChannel 调用）
    bool GetProvider(const std::string& service_name,
                     const std::string& method_name,
                     ProviderInfo& provider);

    // 刷新某个 method 的 provider 列表
    void RefreshMethodProviders(const std::string& method_path);

private:
    ServiceDiscovery();
    ~ServiceDiscovery() = default;
    ServiceDiscovery(const ServiceDiscovery&) = delete;
    ServiceDiscovery& operator=(const ServiceDiscovery&) = delete;

    static void MethodChildrenWatcher(zhandle_t* zh, int type, int state,
                                      const char* path, void* watcherCtx);

    static void ProviderDataWatcher(zhandle_t* zh, int type, int state,
                                    const char* path, void* watcherCtx);

private:
    ZkClient m_zkClient;

    // key: "UserServiceRpc.Login"
    std::unordered_map<std::string, std::vector<ProviderInfo>> m_service_cache;

    // 防止重复 watch
    std::unordered_map<std::string, bool> m_watched_methods;

    std::mutex m_mutex;
};