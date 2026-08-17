#pragma once

#include <semaphore.h>
#include <zookeeper/zookeeper.h>
#include <string>

//负载均衡
#include <vector>

// 封装的zk客户端类
class ZkClient
{
public:
    ZkClient();
    ~ZkClient();
    // zkclient启动连接zkserver
    void Start();
    // 在zkserver上根据指定的path创建znode节点
    void Create(const char *path, const char *data, int datalen, int state=0);
    // 根据参数指定的znode节点路径，或者znode节点的值
    std::string GetData(const char *path);
    // 获取指定路径下的所有子节点名称,负载均衡
    std::vector<std::string> GetChildren(const char *path);

    //带 watch 的读取接口
    std::string GetDataWithWatch(const char *path, watcher_fn watcher, void *watcherCtx);
    std::vector<std::string> GetChildrenWithWatch(const char *path, watcher_fn watcher, void *watcherCtx);
    bool ExistsWithWatch(const char *path, watcher_fn watcher, void *watcherCtx);

    zhandle_t* GetHandle() { return m_zhandle; }
private:
    // zk的客户端句柄
    zhandle_t *m_zhandle;
};