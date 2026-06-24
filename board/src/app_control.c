#define _POSIX_C_SOURCE 200809L

#include "app_control.h"
#include "log.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CTRL_LINE_CAP  512u
#define CTRL_RESP_CAP  2048u
#define CTRL_BACKLOG   4
#define CTRL_POLL_MS   200

typedef struct {
    pthread_mutex_t   lock;
    volatile int      stop;
    int               pending;
    app_ctrl_params_t pending_params;
    int               listen_fd;
    app_ctrl_cfg_t    cfg;
} ctrl_state_t;

static ctrl_state_t g_ctrl = {
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
};

/* ---- minimal JSON field extractor ---- */

/*
 * key は引用符付き文字列であること ("\"op\"" など)。
 * 見つかれば "key": の直後（空白スキップ済み）へのポインタを返す。
 */
static const char *find_key(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (p == NULL) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int parse_uint64(const char *json, const char *key, uint64_t *out)
{
    const char *p = find_key(json, key);
    char *end;
    unsigned long long v;
    if (p == NULL) return -1;
    v = strtoull(p, &end, 10);
    if (end == p) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_string(const char *json, const char *key, char *out, size_t outlen)
{
    const char *p = find_key(json, key);
    size_t i = 0;
    if (p == NULL || *p != '"') return -1;
    p++;
    while (*p != '\0' && *p != '"' && i + 1 < outlen) out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

static int parse_float(const char *json, const char *key, float *out)
{
    const char *p = find_key(json, key);
    char *end;
    double v;
    if (p == NULL) return -1;
    v = strtod(p, &end);
    if (end == p) return -1;
    *out = (float)v;
    return 0;
}

static int parse_bool(const char *json, const char *key, int *out)
{
    const char *p = find_key(json, key);
    if (p == NULL) return -1;
    if (strncmp(p, "true",  4) == 0) { *out = 1; return 0; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 0; }
    if (*p == '1') { *out = 1; return 0; }
    if (*p == '0') { *out = 0; return 0; }
    return -1;
}

/* ---- 逐字节读行（控制通路频率极低，开销可忽略）---- */

static ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return (i > 0) ? (ssize_t)i : -1;
        if (c == '\n') { buf[i] = '\0'; return (ssize_t)i; }
        if (c != '\r') buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return (ssize_t)i; /* 行被截断但非空 */
}

static void send_str(int fd, const char *s)
{
    (void)send(fd, s, strlen(s), MSG_NOSIGNAL);
}

/* ---- 单连接分发 ---- */

static void handle_client(int fd)
{
    char line[CTRL_LINE_CAP];
    char resp[CTRL_RESP_CAP];

    while (!g_ctrl.stop) {
        ssize_t n = read_line(fd, line, sizeof(line));
        if (n < 0) break;
        if (n == 0) continue;

        uint64_t req_id = 0;
        char     op[32] = {0};

        (void)parse_uint64(line, "\"id\"", &req_id);
        if (parse_string(line, "\"op\"", op, sizeof(op)) != 0) {
            snprintf(resp, sizeof(resp),
                     "{\"id\":%llu,\"ok\":false,\"error\":\"missing_op\"}\n",
                     (unsigned long long)req_id);
            send_str(fd, resp);
            continue;
        }

        if (strcmp(op, "status") == 0) {
            char body[CTRL_RESP_CAP - 64];
            body[0] = '\0';
            if (g_ctrl.cfg.status_fn != NULL) {
                g_ctrl.cfg.status_fn(g_ctrl.cfg.status_opaque, body, sizeof(body));
            }
            snprintf(resp, sizeof(resp),
                     "{\"id\":%llu,\"ok\":true,\"status\":%s}\n",
                     (unsigned long long)req_id,
                     (body[0] != '\0') ? body : "{}");
            send_str(fd, resp);

        } else if (strcmp(op, "set") == 0) {
            app_ctrl_params_t p = {0};
            float fv;
            int   iv;

            if (parse_float(line, "\"tone_strength\"", &fv) == 0 &&
                fv >= 0.0f && fv <= 1.0f) {
                p.has_tone_strength = 1;
                p.tone_strength     = fv;
            }
            if (parse_bool(line, "\"nn_clut_enabled\"", &iv) == 0) {
                p.has_nn_clut_enabled = 1;
                p.nn_clut_enabled     = iv;
            }
            if (parse_float(line, "\"high_clip_guard\"", &fv) == 0 && fv > 0.0f) {
                p.has_high_clip_guard = 1;
                p.high_clip_guard     = fv;
            }

            pthread_mutex_lock(&g_ctrl.lock);
            if (p.has_tone_strength) {
                g_ctrl.pending_params.has_tone_strength = 1;
                g_ctrl.pending_params.tone_strength     = p.tone_strength;
            }
            if (p.has_nn_clut_enabled) {
                g_ctrl.pending_params.has_nn_clut_enabled = 1;
                g_ctrl.pending_params.nn_clut_enabled     = p.nn_clut_enabled;
            }
            if (p.has_high_clip_guard) {
                g_ctrl.pending_params.has_high_clip_guard = 1;
                g_ctrl.pending_params.high_clip_guard     = p.high_clip_guard;
            }
            g_ctrl.pending = 1;
            pthread_mutex_unlock(&g_ctrl.lock);

            snprintf(resp, sizeof(resp),
                     "{\"id\":%llu,\"ok\":true,\"queued\":true}\n",
                     (unsigned long long)req_id);
            send_str(fd, resp);
            LOG_INFO("[app_ctrl] set queued: strength=%s nn=%s guard=%s",
                     p.has_tone_strength    ? "yes" : "-",
                     p.has_nn_clut_enabled  ? "yes" : "-",
                     p.has_high_clip_guard  ? "yes" : "-");

        } else {
            snprintf(resp, sizeof(resp),
                     "{\"id\":%llu,\"ok\":false,\"error\":\"unknown_op\"}\n",
                     (unsigned long long)req_id);
            send_str(fd, resp);
            LOG_WARN("[app_ctrl] unknown op: %s", op);
        }
    }
}

/* ---- 公开 API ---- */

int app_ctrl_init(const app_ctrl_cfg_t *cfg)
{
    if (cfg == NULL || cfg->sock_path == NULL || cfg->sock_path[0] == '\0') return -1;
    g_ctrl.cfg  = *cfg;
    g_ctrl.stop = 0;
    return 0;
}

void *app_ctrl_worker(void *arg)
{
    struct sockaddr_un addr;
    int fd;
    int reuse = 1;

    (void)arg;

    /* 确保父目录存在（忽略 EEXIST） */
    {
        const char *last = strrchr(g_ctrl.cfg.sock_path, '/');
        if (last != NULL) {
            char dir[108];
            size_t dlen = (size_t)(last - g_ctrl.cfg.sock_path);
            if (dlen > 0 && dlen < sizeof(dir)) {
                memcpy(dir, g_ctrl.cfg.sock_path, dlen);
                dir[dlen] = '\0';
                (void)mkdir(dir, 0755);
            }
        }
    }

    (void)unlink(g_ctrl.cfg.sock_path);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERR("[app_ctrl] socket: %s", strerror(errno));
        return NULL;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_ctrl.cfg.sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, CTRL_BACKLOG) != 0) {
        LOG_ERR("[app_ctrl] bind/listen %s: %s", g_ctrl.cfg.sock_path, strerror(errno));
        close(fd);
        return NULL;
    }
    (void)chmod(g_ctrl.cfg.sock_path, 0666); /* 允许非 root 的 web 进程连接 */
    g_ctrl.listen_fd = fd;
    LOG_INFO("[app_ctrl] listening on %s", g_ctrl.cfg.sock_path);

    while (!g_ctrl.stop) {
        struct pollfd pfd = {fd, POLLIN, 0};
        int r = poll(&pfd, 1, CTRL_POLL_MS);
        if (r <= 0) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;
        handle_client(client);
        close(client);
    }

    close(fd);
    (void)unlink(g_ctrl.cfg.sock_path);
    g_ctrl.listen_fd = -1;
    LOG_INFO("[app_ctrl] stopped");
    return NULL;
}

int app_ctrl_drain(app_ctrl_params_t *out)
{
    int pending;
    if (out == NULL) return 0;
    pthread_mutex_lock(&g_ctrl.lock);
    pending = g_ctrl.pending;
    if (pending) {
        *out            = g_ctrl.pending_params;
        g_ctrl.pending_params = (app_ctrl_params_t){0};
        g_ctrl.pending  = 0;
    }
    pthread_mutex_unlock(&g_ctrl.lock);
    return pending;
}

void app_ctrl_push(const app_ctrl_params_t *p)
{
    if (p == NULL) return;
    pthread_mutex_lock(&g_ctrl.lock);
    if (p->has_tone_strength) {
        g_ctrl.pending_params.has_tone_strength = 1;
        g_ctrl.pending_params.tone_strength     = p->tone_strength;
    }
    if (p->has_nn_clut_enabled) {
        g_ctrl.pending_params.has_nn_clut_enabled = 1;
        g_ctrl.pending_params.nn_clut_enabled     = p->nn_clut_enabled;
    }
    if (p->has_high_clip_guard) {
        g_ctrl.pending_params.has_high_clip_guard = 1;
        g_ctrl.pending_params.high_clip_guard     = p->high_clip_guard;
    }
    g_ctrl.pending = 1;
    pthread_mutex_unlock(&g_ctrl.lock);
}

void app_ctrl_stop(void)
{
    g_ctrl.stop = 1;
}

void app_ctrl_deinit(void)
{
    g_ctrl.stop    = 0;
    g_ctrl.pending = 0;
    memset(&g_ctrl.pending_params, 0, sizeof(g_ctrl.pending_params));
    memset(&g_ctrl.cfg, 0, sizeof(g_ctrl.cfg));
}
