#include "zookeeperutil.h"
#include "mprpcapplication.h"
#include <semaphore.h>
#include <iostream>

// 全局的watcher观察器   zkserver给zkclient的通知
void global_watcher(zhandle_t *zh, int type,
                   int state, const char *path, void *watcherCtx)
{
    if (type == ZOO_SESSION_EVENT)  // 回调的消息类型是和会话相关的消息类型
	{
		if (state == ZOO_CONNECTED_STATE)  // zkclient和zkserver连接成功
		{
			sem_t *sem = (sem_t*)zoo_get_context(zh);//把信号量加一，才连接真正成功
            if (sem != nullptr)
            {
                sem_post(sem);
            }
		}
	}
}

ZkClient::ZkClient() : m_zhandle(nullptr), m_connect_sem_initialized(false)
{
}

ZkClient::~ZkClient()
{
    if (m_zhandle != nullptr)
    {
        zookeeper_close(m_zhandle); // 关闭句柄，释放资源  MySQL_Conn
    }
    if (m_connect_sem_initialized)
    {
        sem_destroy(&m_connect_sem);
    }
}

// 连接zkserver
void ZkClient::Start()
{
    if (m_zhandle != nullptr)
    {
        return; // 避免重复连接
    }
    std::string host = MprpcApplication::GetInstance().GetConfig().Load("zookeeperip");
    std::string port = MprpcApplication::GetInstance().GetConfig().Load("zookeeperport");
    std::string connstr = host + ":" + port;
    
	/*
	zookeeper_mt：多线程版本
	zookeeper提供了三个线程
	API调用线程 （当前线程）
	网络I/O线程  pthread_create  poll
	watcher回调线程 pthread_create
	*/
	//zk官方函数，init完不代表成功，回调才成功
    m_zhandle = zookeeper_init(connstr.c_str(), global_watcher, 30000, nullptr, nullptr, 0);
    if (nullptr == m_zhandle) 
    {
        std::cout << "zookeeper_init error!" << std::endl;
        exit(EXIT_FAILURE);
    }

    sem_init(&m_connect_sem, 0, 0);
    m_connect_sem_initialized = true;
    zoo_set_context(m_zhandle, &m_connect_sem);

    sem_wait(&m_connect_sem);
    std::cout << "zookeeper_init success!" << std::endl;
}

void ZkClient::Create(const char *path, const char *data, int datalen, int state)
{
    char path_buffer[128];
    int bufferlen = sizeof(path_buffer);
	//负载均衡
	// 如果是顺序节点，直接创建，不做 exists 判断
    if (state & ZOO_SEQUENCE)
    {
        int flag = zoo_create(m_zhandle, path, data, datalen,
                              &ZOO_OPEN_ACL_UNSAFE, state,
                              path_buffer, bufferlen);
        if (flag == ZOK)
        {
            std::cout << "znode create success... path:" << path_buffer << std::endl;
        }
        else
        {
            std::cout << "flag:" << flag << std::endl;
            std::cout << "znode create error... path:" << path << std::endl;
            exit(EXIT_FAILURE);
        }
        return;
    }

    // 普通节点：先判断是否存在，避免重复创建
    int flag = zoo_exists(m_zhandle, path, 0, nullptr);
    if (ZNONODE == flag)
    {
        flag = zoo_create(m_zhandle, path, data, datalen,
                          &ZOO_OPEN_ACL_UNSAFE, state,
                          path_buffer, bufferlen);
        if (flag == ZOK)
        {
            std::cout << "znode create success... path:" << path << std::endl;
        }
        else
        {
            std::cout << "flag:" << flag << std::endl;
            std::cout << "znode create error... path:" << path << std::endl;
            exit(EXIT_FAILURE);
        }
    }
    // int flag;
	// // 先判断path表示的znode节点是否存在，如果存在，就不再重复创建了
	// flag = zoo_exists(m_zhandle, path, 0, nullptr);
	// if (ZNONODE == flag) // 表示path的znode节点不存在
	// {
	// 	// 创建指定path的znode节点了
	// 	flag = zoo_create(m_zhandle, path, data, datalen,
	// 		&ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
	// 	if (flag == ZOK)
	// 	{
	// 		std::cout << "znode create success... path:" << path << std::endl;
	// 	}
	// 	else
	// 	{
	// 		std::cout << "flag:" << flag << std::endl;
	// 		std::cout << "znode create error... path:" << path << std::endl;
	// 		exit(EXIT_FAILURE);
	// 	}
	// }
}

// 根据指定的path，获取znode节点的值
std::string ZkClient::GetData(const char *path)
{
    char buffer[64];
	int bufferlen = sizeof(buffer);
	int flag = zoo_get(m_zhandle, path, 0, buffer, &bufferlen, nullptr);
	if (flag != ZOK)
	{
		std::cout << "get znode error... path:" << path << std::endl;
		return "";
	}
	else
	{
		return buffer;
	}
}

// 获取指定path下的所有子节点，负载均衡
std::vector<std::string> ZkClient::GetChildren(const char *path)
{
    std::vector<std::string> children;

    struct String_vector str_vec;
    str_vec.count = 0;
    str_vec.data = nullptr;

    int flag = zoo_get_children(m_zhandle, path, 0, &str_vec);
    if (flag != ZOK)
    {
        std::cout << "get children error... path:" << path << std::endl;
        return children;
    }

    for (int i = 0; i < str_vec.count; ++i)
    {
        children.push_back(str_vec.data[i]);
    }

    deallocate_String_vector(&str_vec);
    return children;
}

std::string ZkClient::GetDataWithWatch(const char *path, watcher_fn watcher, void *watcherCtx)
{
    char buffer[128] = {0};
    int bufferlen = sizeof(buffer);

    int flag = zoo_wget(m_zhandle, path, watcher, watcherCtx, buffer, &bufferlen, nullptr);
    if (flag != ZOK)
    {
        std::cout << "get znode with watch error... path:" << path << " flag:" << flag << std::endl;
        return "";
    }

    return std::string(buffer, bufferlen);
}

std::vector<std::string> ZkClient::GetChildrenWithWatch(const char *path, watcher_fn watcher, void *watcherCtx)
{
    std::vector<std::string> children;

    struct String_vector str_vec;
    str_vec.count = 0;
    str_vec.data = nullptr;

    int flag = zoo_wget_children(m_zhandle, path, watcher, watcherCtx, &str_vec);
    if (flag != ZOK)
    {
        std::cout << "get children with watch error... path:" << path << " flag:" << flag << std::endl;
        return children;
    }

    for (int i = 0; i < str_vec.count; ++i)
    {
        children.push_back(str_vec.data[i]);
    }

    deallocate_String_vector(&str_vec);
    return children;
}

bool ZkClient::ExistsWithWatch(const char *path, watcher_fn watcher, void *watcherCtx)
{
    int flag = zoo_wexists(m_zhandle, path, watcher, watcherCtx, nullptr);
    return flag == ZOK;
}
