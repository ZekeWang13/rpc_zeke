#include "servicediscovery.h"
#include "logger.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <random>
#include <unistd.h>

ServiceDiscovery& ServiceDiscovery::GetInstance()
{
    static ServiceDiscovery instance;
    return instance;
}

ServiceDiscovery::ServiceDiscovery()
{
}

namespace
{
size_t SelectProviderIndex(size_t count)
{
    // rand() has shared mutable state and races under a multithreaded load generator.
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, count - 1);
    return distribution(engine);
}
}

void ServiceDiscovery::Start()
{
    m_zkClient.Start();
}

void ServiceDiscovery::WatchMethod(const std::string& service_name, const std::string& method_name)
{
    std::string key = service_name + "." + method_name;
    std::string method_path = "/" + service_name + "/" + method_name;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_watched_methods[key])
        {
            return;
        }
        m_watched_methods[key] = true;
    }

    RefreshMethodProviders(method_path);
    LOG_INFO("watch method success: %s", method_path.c_str());
}

bool ServiceDiscovery::GetProvider(const std::string& service_name,
                                   const std::string& method_name,
                                   ProviderInfo& provider)
{
    std::string key = service_name + "." + method_name;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_service_cache.find(key);
        if (it != m_service_cache.end() && !it->second.empty())
        {
            const auto& providers = it->second;
            provider = providers[SelectProviderIndex(providers.size())];
            return true;
        }
    }

    // 缓存不存在时，先初始化 watch 和缓存，再取一次
    WatchMethod(service_name, method_name);

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_service_cache.find(key);
    if (it == m_service_cache.end() || it->second.empty())
    {
        return false;
    }

    const auto& providers = it->second;
    provider = providers[SelectProviderIndex(providers.size())];
    return true;
}

void ServiceDiscovery::RefreshMethodProviders(const std::string& method_path)
{
    // 1. 重新注册 method_path 的子节点 watch，并取最新 provider 节点列表
    std::vector<std::string> children =
        m_zkClient.GetChildrenWithWatch(method_path.c_str(), MethodChildrenWatcher, this);

    std::sort(children.begin(), children.end());

    std::vector<ProviderInfo> providers;
    for (const auto& child : children)
    {
        std::string provider_path = method_path + "/" + child;

        // 2. 读取 provider data，并给每个 provider 节点挂 data watch
        std::string host_data =
            m_zkClient.GetDataWithWatch(provider_path.c_str(), ProviderDataWatcher, this);

        if (!host_data.empty())
        {
            ProviderInfo info;
            info.node_name = child;
            info.host_data = host_data;
            providers.push_back(info);
        }
    }

    // method_path: /UserServiceRpc/Login
    // key: UserServiceRpc.Login
    std::string key = method_path;
    if (!key.empty() && key[0] == '/')
    {
        key.erase(0, 1);
    }
    std::replace(key.begin(), key.end(), '/', '.');

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_service_cache[key] = providers;
    }

    LOG_INFO("refresh providers success, method:%s count:%d",
             method_path.c_str(), (int)providers.size());
}

void ServiceDiscovery::MethodChildrenWatcher(zhandle_t* zh, int type, int state,
                                             const char* path, void* watcherCtx)
{
    if (watcherCtx == nullptr || path == nullptr)
    {
        return;
    }

    ServiceDiscovery* self = static_cast<ServiceDiscovery*>(watcherCtx);

    // 关心子节点变化 / 节点创建 / 节点删除
    if (type == ZOO_CHILD_EVENT ||
        type == ZOO_CREATED_EVENT ||
        type == ZOO_DELETED_EVENT)
    {
        LOG_INFO("method children changed, path:%s type:%d", path, type);
        self->RefreshMethodProviders(path);
    }
}

void ServiceDiscovery::ProviderDataWatcher(zhandle_t* zh, int type, int state,
                                           const char* path, void* watcherCtx)
{
    if (watcherCtx == nullptr || path == nullptr)
    {
        return;
    }

    ServiceDiscovery* self = static_cast<ServiceDiscovery*>(watcherCtx);

    // provider data 改变、节点删除、节点创建时重新刷新整个 method
    if (type == ZOO_CHANGED_EVENT ||
        type == ZOO_DELETED_EVENT ||
        type == ZOO_CREATED_EVENT)
    {
        std::string provider_path = path;
        size_t pos = provider_path.find_last_of('/');
        if (pos == std::string::npos)
        {
            return;
        }

        std::string method_path = provider_path.substr(0, pos);

        LOG_INFO("provider data changed, provider:%s method:%s type:%d",
                 path, method_path.c_str(), type);

        self->RefreshMethodProviders(method_path);
    }
}
