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

    // 2. 初始化日志系统
    log_init();

    // 3. 创建epoll实例
    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        log_error("epoll_create1 failed");
        exit(1);
    }

    // 4. 创建监听套接字
    int listen_fd = create_listen_fd();

    // 5. 初始化客户端数组
    client_init();

    // 6. 将监听fd加入epoll监控，关注可读事件（新连接）
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

    // 7. 打印启动信息
    printf("Server running on http://127.0.0.1:%d\n", PORT);
    printf("Static root: %s\n", DOC_ROOT);
    printf("Connection timeout: %ds\n", CONN_TIMEOUT);
    printf("Log file: %s\n", LOG_FILE);
    printf("================================================\n");

    // ===== epoll主事件循环（程序核心死循环） =====
    while (1)
    {
        // epoll_wait 超时设为1000ms（1秒）
        // 作用：无事件时每秒返回一次，用于检查超时连接
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (nfds < 0)
        {
            log_error("epoll_wait error");
            continue;
        }

        // ===== 每次循环检查一次超时连接 =====
        check_timeout(epfd);

        // ===== 处理所有就绪事件 =====
        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // ==========================================
            // 分支1：fd == 监听fd → 有新客户端发起TCP连接
            // ==========================================
            if (fd == listen_fd)
            {
                struct sockaddr_in cli_addr;
                socklen_t cli_len = sizeof(cli_addr);

                // 取出完成三次握手的客户端
                int cfd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);
                if (cfd < 0)
                {
                    log_error("accept failed");
                    continue;
                }

                // 设置非阻塞
                set_nonblock(cfd);

                // 找空闲槽位
                int slot = get_free_client();
                if (slot == -1)
                {
                    // 并发达到上限，直接拒绝
                    log_error("too many connections, refused");
                    close(cfd);
                    continue;
                }

                // 保存客户端信息
                Client *cli = &client_arr[slot];
                cli->fd = cfd;
                cli->buf_len = 0;
                cli->last_time = time(NULL); // 记录连接建立时间
                // 将二进制IP转换为字符串
                inet_ntop(AF_INET, &cli_addr.sin_addr, cli->ip, sizeof(cli->ip));

                // 将新客户端fd加入epoll监控
                ev.data.fd = cfd;
                ev.events = EPOLLIN;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

                // 日志：新连接建立
                char msg[128];
                snprintf(msg, sizeof(msg), "new connection from %s", cli->ip);
                log_error(msg);
            }
            // ==========================================
            // 分支2：普通客户端fd → 浏览器发来HTTP请求数据
            // ==========================================
            else
            {
                // 根据fd找到对应的客户端槽位
                int slot = find_client_by_fd(fd);
                if (slot == -1)
                    continue;

                Client *cli = &client_arr[slot];

                // 更新最后活动时间（有数据到来）
                cli->last_time = time(NULL);

                // ===== 请求长度限制检查 =====
                // 计算缓冲区剩余空间
                int remain = BUF_SIZE - cli->buf_len - 1;
                if (remain <= 0)
                {
                    // 缓冲区已满，请求过大，直接断开连接
                    // 防止恶意超大请求导致缓冲区溢出
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "request too large from %s, closing", cli->ip);
                    log_error(msg);
                    close_client(epfd, slot);
                    continue;
                }

                // ===== 读取客户端数据 =====
                // 追加到缓冲区尾部，解决TCP粘包/分包
                int n = recv(fd, cli->buf + cli->buf_len, remain, 0);
                if (n <= 0)
                {
                    // n=0：客户端正常关闭
                    // n<0：网络异常
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "connection closed: %s", cli->ip);
                    log_error(msg);
                    close_client(epfd, slot);
                    continue;
                }

                // 更新缓冲区有效长度
                cli->buf_len += n;

                // ===== 解析HTTP请求 =====
                HttpRequest req;
                http_req_init(&req);
                int ret = parse_http(cli->buf, cli->buf_len, &req);
                if (ret == 0)
                {
                    // 记录响应开始时间
                    long start = get_millis();

                    // 发送HTTP响应（读取文件 + sendfile）
                    send_response(fd, &req);

                    // 计算响应耗时（毫秒）
                    long cost = get_millis() - start;

                    // 记录访问日志（IP + 方法 + URL + 状态码 + 耗时）
                    log_access(cli->ip, req.method, req.url,
                               req.status_code, cost);

                    // ===== 长短连接处理 =====
                    if (!req.keep_alive)
                    {
                        // 短连接：响应发送完毕直接关闭
                        close_client(epfd, slot);
                    }
                    else
                    {
                        // 长连接：不清fd，只清空缓冲区，等待下一次请求
                        cli->buf_len = 0;
                        memset(cli->buf, 0, BUF_SIZE);
                    }
                }
            }
        }
    }

}