#ifndef LOG_H
#define LOG_H

#include "http.h"

/**
 * @brief 初始化日志系统，打开日志文件
 * 以追加模式打开，服务重启不丢失历史日志
 */
void log_init();

/**
 * @brief 记录一条访问日志，同时输出到控制台和文件
 * @param ip      客户端IP地址
 * @param method  请求方法（GET/POST）
 * @param url     请求路径
 * @param status  响应状态码（200/404）
 * @param cost_ms 响应耗时（毫秒）
 */
void log_access(const char *ip, const char *method, const char *url,
                int status, long cost_ms);

/**
 * @brief 记录一条错误/信息日志
 * @param msg 日志消息内容
 */
void log_error(const char *msg);

#endif