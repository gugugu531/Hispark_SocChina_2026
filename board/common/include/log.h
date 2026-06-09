#ifndef SOCCHINA_LOG_H
#define SOCCHINA_LOG_H

#include <stdio.h>

/* 统一日志（禁止散落裸 printf）。 */
#define LOG_INFO(fmt, ...) fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)  fprintf(stderr, "[ERR ] " fmt "\n", ##__VA_ARGS__)

/* 海思 MPI / ACL 返回值检查：非 0 即打印错误码与表达式。 */
#define CHECK_RET(expr)                                          \
    do {                                                         \
        int _ret = (expr);                                       \
        if (_ret != 0) {                                         \
            LOG_ERR("%s failed: %#x", #expr, (unsigned)_ret);    \
        }                                                        \
    } while (0)

#endif /* SOCCHINA_LOG_H */
