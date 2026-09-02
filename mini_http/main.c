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

