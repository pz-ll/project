// 标准库头文件
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

// ===== 全局宏定义 =====
#define MAX_EVENTS 1024    // epoll最大并发事件数，限制同时在线客户端
#define PORT 8080          // 服务监听端口
#define BUF_SIZE 1024      // 接收缓冲区大小（字节）
#define DOC_ROOT "./www/"  // 静态资源根目录
#define CONN_TIMEOUT 120    // 连接超时时间（秒），闲置超过自动断开
#define LOG_FILE "./log.txt" // 日志文件路径

/**
 * @brief 解析完成后的HTTP请求结构体
 * 存储从原始报文中提取的所有关键字段
 */
typedef struct HttpRequest {
    char method[32];       // 请求方法：GET / POST / HEAD
    char url[128];         // 请求资源路径，如 /index.html
    char host[128];        // Host请求头：域名/IP
    char user_agent[256];  // User-Agent请求头：浏览器信息
    int keep_alive;        // 长连接标记：1=开启 0=关闭（短连接）
    int status_code;       // 响应状态码：200=成功 404=未找到
} HttpRequest;

// ===== HTTP协议层函数 =====
void http_req_init(HttpRequest *req);   // 初始化请求结构体，清空所有字段
int parse_http(char *buf, int len, HttpRequest *req); // 解析原始HTTP报文
void send_response(int fd, HttpRequest *req); // 根据请求生成并发送响应
void set_nonblock(int fd);              // 将fd设置为非阻塞IO
