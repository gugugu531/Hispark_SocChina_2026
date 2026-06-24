#define _POSIX_C_SOURCE 200809L

#include "app_control.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TEST_SOCK "/tmp/test-app-control.sock"

/* ---- 测试辅助 ---- */

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static int connect_sock(void) {
    struct sockaddr_un addr = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEST_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 发一行请求，读响应直到 \n，返回 buf 长度。 */
static ssize_t transact(int fd, const char *req, char *buf, size_t cap) {
    if (send(fd, req, strlen(req), 0) < 0) return -1;
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        if (c == '\n') { buf[i] = '\0'; return (ssize_t)i; }
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return (ssize_t)i;
}

/* ---- 状态回调 stub ---- */

static void stub_status(void *opaque, char *buf, size_t buflen) {
    (void)opaque;
    snprintf(buf, buflen, "{\"state\":\"RUNNING\",\"display_fps\":30.0}");
}

/* ---- worker 线程 ---- */

static pthread_t g_worker;

static void start_worker(void) {
    app_ctrl_cfg_t cfg = {
        .sock_path    = TEST_SOCK,
        .status_fn    = stub_status,
        .status_opaque = NULL,
    };
    assert(app_ctrl_init(&cfg) == 0);
    assert(pthread_create(&g_worker, NULL, app_ctrl_worker, NULL) == 0);
    sleep_ms(50); /* ソケット作成待ち */
}

static void stop_worker(void) {
    app_ctrl_stop();
    pthread_join(g_worker, NULL);
    app_ctrl_deinit();
}

/* ---- テスト1: status クエリ ---- */

static void test_status(void) {
    int fd = connect_sock();
    assert(fd >= 0);

    char resp[512];
    ssize_t n = transact(fd, "{\"id\":1,\"op\":\"status\"}\n", resp, sizeof(resp));
    assert(n > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"status\"") != NULL);
    assert(strstr(resp, "RUNNING") != NULL);
    assert(strstr(resp, "\"id\":1") != NULL);
    close(fd);
    puts("PASS test_status");
}

/* ---- テスト2: set パラメータ + drain ---- */

static void test_set_and_drain(void) {
    int fd = connect_sock();
    assert(fd >= 0);

    char resp[512];
    ssize_t n = transact(
        fd,
        "{\"id\":2,\"op\":\"set\",\"params\":{\"tone_strength\":0.42,\"nn_clut_enabled\":false}}\n",
        resp, sizeof(resp));
    assert(n > 0);
    assert(strstr(resp, "\"ok\":true") != NULL);
    assert(strstr(resp, "\"queued\":true") != NULL);
    close(fd);

    app_ctrl_params_t p = {0};
    int got = app_ctrl_drain(&p);
    assert(got == 1);
    assert(p.has_tone_strength == 1);
    assert(p.tone_strength > 0.41f && p.tone_strength < 0.43f);
    assert(p.has_nn_clut_enabled == 1);
    assert(p.nn_clut_enabled == 0);
    assert(p.has_high_clip_guard == 0);

    /* 二回目は空 */
    got = app_ctrl_drain(&p);
    assert(got == 0);
    puts("PASS test_set_and_drain");
}

/* ---- テスト3: last-write-wins ---- */

static void test_last_write_wins(void) {
    int fd1 = connect_sock();
    assert(fd1 >= 0);

    char resp[512];
    /* tone_strength: 0.1 */
    (void)transact(fd1,
                   "{\"id\":3,\"op\":\"set\",\"params\":{\"tone_strength\":0.1}}\n",
                   resp, sizeof(resp));
    close(fd1);

    /* 즉시 같은 파라미터를 0.9로 덮어씀 */
    int fd2 = connect_sock();
    assert(fd2 >= 0);
    (void)transact(fd2,
                   "{\"id\":4,\"op\":\"set\",\"params\":{\"tone_strength\":0.9}}\n",
                   resp, sizeof(resp));
    close(fd2);

    app_ctrl_params_t p = {0};
    int got = app_ctrl_drain(&p);
    assert(got == 1);
    assert(p.has_tone_strength == 1);
    /* last-write-wins: 0.9 が残る */
    assert(p.tone_strength > 0.89f && p.tone_strength < 0.91f);
    puts("PASS test_last_write_wins");
}

/* ---- テスト4: unknown_op ---- */

static void test_unknown_op(void) {
    int fd = connect_sock();
    assert(fd >= 0);

    char resp[512];
    ssize_t n = transact(fd, "{\"id\":5,\"op\":\"reboot\"}\n", resp, sizeof(resp));
    assert(n > 0);
    assert(strstr(resp, "\"ok\":false") != NULL);
    assert(strstr(resp, "unknown_op") != NULL);
    close(fd);
    puts("PASS test_unknown_op");
}

/* ---- テスト5: high_clip_guard 範囲チェック ---- */

static void test_guard_validation(void) {
    int fd = connect_sock();
    assert(fd >= 0);

    char resp[512];
    /* guard = -1.0 は無効（> 0.0 が必須） */
    (void)transact(fd,
                   "{\"id\":6,\"op\":\"set\",\"params\":{\"high_clip_guard\":-1.0}}\n",
                   resp, sizeof(resp));
    close(fd);

    app_ctrl_params_t p = {0};
    int got = app_ctrl_drain(&p);
    /* 無効値は無視されるので pending は立たないはず */
    (void)got;
    assert(p.has_high_clip_guard == 0);
    puts("PASS test_guard_validation");
}

int main(void) {
    start_worker();

    test_status();
    test_set_and_drain();
    test_last_write_wins();
    test_unknown_op();
    test_guard_validation();

    stop_worker();
    puts("all app_control tests passed");
    return 0;
}
