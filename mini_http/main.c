#include "http.h"
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>

/**
 * @brief 客户端连接结构体
 * 每个在线客户端对应一个slot，存储连接相关所有状态
 */
typedef struct Client {
    int fd;                  // 客户端套接字fd，-1表示该槽位空闲
    char buf[BUF_SIZE];      // 接收HTTP报文的缓冲区
    int buf_len;             // 缓冲区当前有效数据长度
    char ip[32];             // 客户端IP地址字符串
    time_t last_time;        // 最后一次活动时间戳（秒），用于超时判断
} Client;

// 全局客户端数组，统一管理所有在线连接
static Client client_arr[MAX_EVENTS];


/**
 * @brief 创建TCP监听套接字
 * @return 监听fd
 *
 * 完整流程：socket → setsockopt端口复用 → bind → listen → 非阻塞
 */
static int create_listen_fd()
{
    // 1. 创建TCP流式套接字
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        log_error("socket create failed");
        exit(1);
    }

    // 2. 设置端口复用，服务重启无需等待TIME_WAIT超时
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 填充地址结构体
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT); // 端口转网络字节序

    // 绑定 0.0.0.0，监听本机所有网卡
    int ret = inet_pton(AF_INET, "0.0.0.0", &addr.sin_addr);
    if (ret <= 0)
    {
        log_error("inet_pton error");
        close(fd);
        exit(1);
    }

    // 4. 绑定IP+端口
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        log_error("bind failed");
        close(fd);
        exit(1);
    }

    // 5. 开始监听，半连接队列长度128
    if (listen(fd, 128) < 0)
    {
        log_error("listen failed");
        close(fd);
        exit(1);
    }

    // 6. 设置为非阻塞IO
    set_nonblock(fd);
    return fd;
}

/**
 * @brief 初始化客户端数组，全部标记为空闲
 */
static void client_init()
{
    for (int i = 0; i < MAX_EVENTS; i++)
    {
        client_arr[i].fd = -1;       // fd=-1 表示空闲
        client_arr[i].buf_len = 0;
        client_arr[i].last_time = 0;
    }
}

/**
 * @brief 查找一个空闲的客户端槽位
 * @return 空闲下标，-1表示槽位已满
 */
static int get_free_client()
{
    for (int i = 0; i < MAX_EVENTS; i++)
    {
        if (client_arr[i].fd == -1)
            return i;
    }
    return -1;
}

/**
 * @brief 根据fd值查找对应的客户端下标
 * @param fd 客户端套接字
 * @return 数组下标，-1未找到
 *
 * 从epoll事件中拿到fd后，需要反向找到对应的Client结构体
 */
static int find_client_by_fd(int fd)
{
    for (int i = 0; i < MAX_EVENTS; i++)
    {
        if (client_arr[i].fd == fd)
            return i;
    }
    return -1;
}

/**
 * @brief 关闭客户端连接，释放所有资源
 * @param epfd epoll实例fd
 * @param slot 客户端数组下标
 *
 * 统一的连接清理函数，避免代码重复：
 * 1. 从epoll移除  2. 关闭套接字  3. 重置结构体字段
 */
static void close_client(int epfd, int slot)
{
    Client *cli = &client_arr[slot];
    if (cli->fd < 0)
        return; // 已经关闭了，直接返回

    // 从epoll监控中移除
    epoll_ctl(epfd, EPOLL_CTL_DEL, cli->fd, NULL);
    // 关闭套接字
    close(cli->fd);
    // 重置所有字段，标记为空闲
    cli->fd = -1;
    cli->buf_len = 0;
    memset(cli->buf, 0, BUF_SIZE);
    cli->last_time = 0;
    cli->ip[0] = '\0';
}

/**
 * @brief 检查并清理超时连接
 * @param epfd epoll实例fd
 *
 * 遍历所有客户端，最后活动时间超过 CONN_TIMEOUT 秒的自动断开
 * 防止长连接客户端长期闲置占用文件描述符
 * 每秒由epoll_wait超时返回时触发一次
 */
static void check_timeout(int epfd)
{
    time_t now = time(NULL);
    int count = 0; // 统计本次清理的连接数

    for (int i = 0; i < MAX_EVENTS; i++)
    {
        if (client_arr[i].fd >= 0) // 只检查在线连接
        {
            // 当前时间 - 最后活动时间 > 超时阈值
            if (now - client_arr[i].last_time > CONN_TIMEOUT)
            {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "timeout close: %s (idle %ds)",
                         client_arr[i].ip, CONN_TIMEOUT);
                log_error(msg);
                close_client(epfd, i);
                count++;
            }
        }
    }

    // 如果清理了连接，打印汇总
    if (count > 0)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "cleanup %d timeout connections", count);
        log_error(msg);
    }
}

/**
 * @brief 获取当前毫秒级时间戳
 * @return 毫秒数
 *
 * 用于计算请求响应耗时
 * gettimeofday 精度到微秒，这里转为毫秒
 */
static long get_millis()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


/**
 * @brief 主函数：程序入口
 */
int main()
{
    // ===== 初始化阶段 =====

    // 1. 确保静态资源目录存在
    system("mkdir -p ./www");

    // 2. 创建epoll实例
    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        log_error("epoll_create1 failed");
        exit(1);
    }

    // 3. 创建监听套接字
    int listen_fd = create_listen_fd();

    // 4. 初始化客户端数组
    client_init();

    // 5. 将监听fd加入epoll监控，关注可读事件（新连接）
    struct epoll_event ev, events[MAX_EVENTS];
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0)
    {
        log_error("epoll_ctl add listen failed");
        close(listen_fd);
        close(epfd);
        exit(1);
    }

    // 6. 打印启动信息
    printf("Server running on http://127.0.0.1:%d\n", PORT);
    printf("Static root: %s\n", DOC_ROOT);
    printf("Connection timeout: %ds\n", CONN_TIMEOUT);
    printf("Log file: %s\n", LOG_FILE);
    printf("================================================\n");


}