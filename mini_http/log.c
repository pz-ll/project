#include "http.h"

// 日志文件指针，全局静态变量，模块内共享
static FILE *log_fp = NULL;

/**
 * @brief 获取当前时间的格式化字符串
 * @param buf 输出缓冲区
 * @param len 缓冲区大小
 * 输出格式：2026-08-10 14:30:01
 */
static void get_time_str(char *buf, int len)
{
    time_t now = time(NULL);           // 获取当前时间戳
    struct tm *t = localtime(&now);    // 转换为本地时间结构体
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             t->tm_year + 1900,        // 年份从1900开始计数，需+1900
             t->tm_mon + 1,            // 月份0-11，需+1
             t->tm_mday,               // 日期
             t->tm_hour,               // 时
             t->tm_min,                // 分
             t->tm_sec);               // 秒
}

/**
 * @brief 初始化日志系统
 * 以追加模式打开日志文件，服务重启后历史日志保留
 * 同时写入一条服务启动标记
 */
void log_init()
{
    log_fp = fopen(LOG_FILE, "a"); // "a" = append追加模式
    if (!log_fp)
    {
        perror("log file open failed");
        return;
    }

    // 写入服务启动分隔线，方便区分每次运行
    char time_str[64];
    get_time_str(time_str, sizeof(time_str));
    fprintf(log_fp, "\n===== Server Start %s =====\n", time_str);
    fflush(log_fp); // 立即刷新到磁盘，防止缓冲丢失
}

/**
 * @brief 记录访问日志，控制台+文件双输出
 * @param ip      客户端IP
 * @param method  请求方法
 * @param url     请求路径
 * @param status  响应状态码
 * @param cost_ms 响应耗时（毫秒）
 *
 * 控制台带颜色：2xx绿色，4xx/5xx红色
 * 文件中不带颜色转义码，纯文本
 */
void log_access(const char *ip, const char *method, const char *url,
                int status, long cost_ms)
{
    char time_str[64];
    get_time_str(time_str, sizeof(time_str));

    // 根据状态码选择控制台颜色
    const char *color = "\033[32m"; // 默认绿色：2xx成功
    if (status >= 400)
        color = "\033[31m";         // 红色：4xx/5xx错误

    // ===== 控制台输出（带颜色） =====
    // 格式：时间  IP  方法 URL  状态码  耗时
    printf("%s%s\033[0m  %s  %s %s  %s%d\033[0m  %ldms\n",
           color, time_str,    // 时间（带颜色）
           ip,                 // 客户端IP
           method, url,        // 请求方法 + 路径
           color, status,      // 状态码（带颜色）
           cost_ms);           // 响应耗时

    // ===== 文件输出（纯文本，不带颜色） =====
    if (log_fp)
    {
        fprintf(log_fp, "%s  %s  %s %s  %d  %ldms\n",
                time_str, ip, method, url, status, cost_ms);
        fflush(log_fp); // 每次写入立即刷新，防止崩溃丢失日志
    }
}

/**
 * @brief 记录错误/信息日志
 * @param msg 日志内容
 *
 * 用于记录连接建立、断开、超时、异常等事件
 * 控制台红色显示，文件中带 [ERROR] 标记
 */
void log_error(const char *msg)
{
    char time_str[64];
    get_time_str(time_str, sizeof(time_str));

    // 控制台红色输出
    printf("\033[31m%s  [INFO] %s\033[0m\n", time_str, msg);

    // 写入文件
    if (log_fp)
    {
        fprintf(log_fp, "%s  [INFO] %s\n", time_str, msg);
        fflush(log_fp);
    }
}