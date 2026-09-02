#include "http.h"

/**
 * @brief 根据文件后缀匹配对应的MIME类型
 * @param filename 文件完整路径
 * @return MIME类型字符串，用于响应头Content-Type
 *
 * 使用 strrchr 找到最后一个小数点，精准匹配文件后缀
 * 避免文件名含多个点时识别错误
 */
static const char* get_mime(const char *filename)
{
    // 找到最后一个 '.' 的位置
    const char *p = strrchr(filename, '.');
    if (!p)
        return "application/octet-stream"; // 无后缀，当作二进制文件

    // 逐一匹配常见后缀
    if (strcmp(p, ".html") == 0 || strcmp(p, ".htm") == 0)
        return "text/html;charset=utf-8";
    if (strcmp(p, ".txt") == 0)
        return "text/plain;charset=utf-8";
    if (strcmp(p, ".jpg") == 0 || strcmp(p, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(p, ".png") == 0)
        return "image/png";
    if (strcmp(p, ".css") == 0)
        return "text/css";
    if (strcmp(p, ".js") == 0)
        return "application/javascript";

    return "application/octet-stream"; // 未知类型
}

/**
 * @brief 发送404 Not Found响应
 * @param fd 客户端套接字
 *
 * 优先读取本地 www/404.html 文件；
 * 文件不存在时使用内置字符串兜底
 */
static void send_404(int fd)
{
    struct stat st;
    const char *path = "./www/404.html";

    // 尝试打开本地404页面文件
    int f = open(path, O_RDONLY);
    if (f >= 0 && stat(path, &st) == 0)
    {
        // ===== 方案A：读取本地404.html文件 =====
        off_t size = st.st_size;
        char head[BUF_SIZE] = {0};

        // 拼接404响应头
        snprintf(head, sizeof(head),
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html;charset=utf-8\r\n"
        "Content-Length: %lld\r\n\r\n", (long long)size);
        send(fd, head, strlen(head), 0);

        // sendfile零拷贝发送文件内容
        sendfile(fd, f, NULL, size);
        close(f);
        return;
    }

    // ===== 方案B：内置兜底404页面（无文件时使用） =====
    char buf404[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type:text/html;charset=utf-8\r\n"
    "Content-Length:22\r\n\r\n"
    "<h1>404 Not Found</h1>";
    send(fd, buf404, strlen(buf404), 0);
}

/**
 * @brief 读取本地文件并发送HTTP 200响应
 * @param fd 客户端套接字
 * @param filepath 经过安全过滤的本地文件路径
 * @return 响应状态码：200成功，404文件不存在
 *
 * 使用 sendfile 系统调用实现零拷贝传输：
 * 数据直接从文件内核缓冲区发到socket，
 * 不需要经过用户态内存拷贝，性能更高
 */
static int send_file(int fd, const char *filepath)
{
    struct stat st;

    // 打开文件 + 获取文件属性
    int f = open(filepath, O_RDONLY);
    if (f < 0 || stat(filepath, &st) < 0)
    {
        // 文件打开失败 → 返回404
        send_404(fd);
        if (f >= 0)
            close(f);
        return 404;
    }

    off_t file_size = st.st_size;         // 文件字节大小
    const char *mime = get_mime(filepath); // 获取MIME类型
    char head[BUF_SIZE] = {0};

    // 拼接标准 200 OK 响应头
    snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n\r\n",
        mime, (long long)file_size);
    send(fd, head, strlen(head), 0);

    // 零拷贝发送文件内容（内核态直接传输，不经过用户缓冲区）
    sendfile(fd, f, NULL, file_size);
    close(f);

    return 200;
}

/**
 * @brief 路径安全过滤，防御目录穿越攻击
 * @param dst 输出：拼接后的完整本地路径
 * @param src 输入：客户端请求的URL路径
 *
 * 1. 拼接根目录前缀 DOC_ROOT，强制锁定在 www/ 内
 * 2. 循环删除所有 "../" 片段，防止攻击者用 ../../etc/passwd 越权访问
 */
static void safe_path(char *dst, const char *src)
{
    // 拼接：根目录 + URL路径
    strcpy(dst, DOC_ROOT);
    strcat(dst, src);

    // 循环清除所有 "../"，直到找不到为止
    char *p = strstr(dst, "../");
    while (p)
    {
        // memmove 内存移动，删除3个字符 "../"
        memmove(p, p + 3, strlen(p + 3) + 1);
        p = strstr(dst, "../"); // 继续查找下一处
    }
}

/**
 * @brief HTTP响应入口函数，串联路径过滤与文件发送
 * @param fd  客户端套接字
 * @param req 请求结构体（函数内会回写 status_code 字段）
 *
 * 流程：根路径跳转 → 安全路径拼接 → 读取文件 → 发送响应
 */
void send_response(int fd, HttpRequest *req)
{
    char filepath[512];

    // 根路径 "/" 自动映射到首页
    if (strcmp(req->url, "/") == 0)
    {
        strcpy(req->url, "/index.html");
    }

    // 安全拼接本地文件路径
    safe_path(filepath, req->url);

    // 发送文件，返回状态码存入req
    req->status_code = send_file(fd, filepath);
}