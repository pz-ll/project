#include "http.h"

/**
 * @brief 初始化HttpRequest结构体，清空所有字段
 * @param req 请求结构体指针
 */
void http_req_init(HttpRequest *req)
{
    memset(req, 0, sizeof(HttpRequest));
    req->keep_alive = 0;   // 默认短连接
    req->status_code = 0;  // 状态码初始为0，由file层填充
}

/**
 * @brief 从请求行提取URL路径
 * @param req_buf 完整原始请求报文
 * @param uri 输出提取到的URL
 *
 * 请求行格式：GET /index.html HTTP/1.1
 * %*s 跳过第一个字符串(GET)，%s 读取第二个字符串(/index.html)
 */
static void get_uri(const char *req_buf, char *uri)
{
    sscanf(req_buf, "%*s %s", uri);
}

/**
 * @brief 从请求行提取请求方法
 * @param req_buf 完整原始请求报文
 * @param method 输出请求方法
 */
static void get_method(const char *req_buf, char *method)
{
    sscanf(req_buf, "%s", method);
}

/**
 * @brief 通用请求头提取函数，从报文中提取指定头部的值
 * @param buf    完整请求报文
 * @param header 请求头名称，如 "Host"、"User-Agent"
 * @param value  输出缓冲区，保存提取到的值
 * @param max_len 输出缓冲区最大长度
 *
 * 原理：在报文中查找 "HeaderName: " 前缀，
 *       从冒号后开始读取直到遇到换行符为止
 */
static void get_header(const char *buf, const char *header, char *value, int max_len)
{
    char pattern[128];
    // 拼接查找模式，如 "Host: "
    snprintf(pattern, sizeof(pattern), "%s: ", header);

    // 在报文中查找该请求头
    char *p = strstr(buf, pattern);
    if (!p)
    {
        value[0] = '\0'; // 没找到，返回空字符串
        return;
    }

    // 指针移到冒号后面，跳过 ": "
    p += strlen(pattern);

    // 逐个字符复制，直到遇到 \r 或 \n（行尾）
    int i = 0;
    while (*p != '\r' && *p != '\n' && i < max_len - 1)
    {
        value[i++] = *p++;
    }
    value[i] = '\0'; // 字符串结尾
}

/**
 * @brief 解析HTTP请求报文，填充HttpRequest结构体
 * @param buf 客户端原始数据缓冲区
 * @param len 缓冲区有效数据长度
 * @param req 待填充的请求结构体
 * @return 0 解析成功
 */
int parse_http(char *buf, int len, HttpRequest *req)
{
    // 先清空结构体，避免脏数据
    http_req_init(req);

    // 1. 解析请求行：方法 + URL
    get_method(buf, req->method);
    get_uri(buf, req->url);

    // 2. 解析常用请求头
    get_header(buf, "Host", req->host, sizeof(req->host));
    get_header(buf, "User-Agent", req->user_agent, sizeof(req->user_agent));

    // 3. 判断是否长连接
    // HTTP/1.1 默认长连接，这里简化：有keep-alive头才认为是长连接
    if (strstr(buf, "Connection: keep-alive"))
        req->keep_alive = 1;
    else
        req->keep_alive = 0;

    return 0;
}

/**
 * @brief 将文件描述符设置为非阻塞IO
 * @param fd 套接字文件描述符
 *
 * epoll必须配合非阻塞fd使用，
 * 否则accept/recv可能阻塞整个事件循环
 */
void set_nonblock(int fd)
{
    int flag = fcntl(fd, F_GETFL, 0);       // 获取当前属性
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);  // 追加非阻塞标记
}