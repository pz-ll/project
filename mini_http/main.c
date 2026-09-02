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