#include "app_control_sock.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_MAX_CLIENTS 4
#define SOCK_BUF_SIZE    4096
#define SOCK_PATH_MAX    108

struct app_control_sock {
    int listen_fd;
    int client_fds[SOCK_MAX_CLIENTS];
    char client_bufs[SOCK_MAX_CLIENTS][SOCK_BUF_SIZE];
    size_t client_len[SOCK_MAX_CLIENTS];
};

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_client(app_control_sock_t *s, int idx) {
    if (s->client_fds[idx] >= 0) {
        close(s->client_fds[idx]);
    }
    s->client_fds[idx] = -1;
    s->client_len[idx] = 0;
}

app_control_sock_t *app_control_sock_start(const char *path) {
    struct sockaddr_un addr;
    app_control_sock_t *s;

    fprintf(stderr, "[app_control] starting socket on %s\n", path ? path : "(null)");
    if (path == NULL || path[0] == '\0' || strlen(path) >= SOCK_PATH_MAX) {
        fprintf(stderr, "[app_control] invalid path\n");
        return NULL;
    }

    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        fprintf(stderr, "[app_control] calloc failed\n");
        return NULL;
    }
    for (int i = 0; i < SOCK_MAX_CLIENTS; i++) {
        s->client_fds[i] = -1;
    }

    s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        fprintf(stderr, "[app_control] socket() failed: %d\n", errno);
        free(s);
        return NULL;
    }
    fprintf(stderr, "[app_control] listen_fd=%d\n", s->listen_fd);

    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(s->listen_fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[app_control] bind failed: %d\n", errno);
        close(s->listen_fd);
        free(s);
        return NULL;
    }
    if (listen(s->listen_fd, 2) != 0) {
        fprintf(stderr, "[app_control] listen failed: %d\n", errno);
        close(s->listen_fd);
        unlink(path);
        free(s);
        return NULL;
    }
    if (set_nonblock(s->listen_fd) != 0) {
        fprintf(stderr, "[app_control] set_nonblock failed: %d\n", errno);
        close(s->listen_fd);
        unlink(path);
        free(s);
        return NULL;
    }
    fprintf(stderr, "[app_control] listening on %s\n", path);
    LOG_INFO("app-control listening on %s", path);
    return s;
}

static int append_char(app_control_sock_t *s, int idx, char c) {
    size_t len = s->client_len[idx];
    if (len + 1 >= SOCK_BUF_SIZE) {
        return -1; /* overflow */
    }
    s->client_bufs[idx][len] = c;
    s->client_len[idx] = len + 1;
    s->client_bufs[idx][len + 1] = '\0';
    if (c == '\n') {
        return 1; /* complete line */
    }
    return 0; /* more to read */
}

static void process_request(app_control_sock_t *s, int idx, const char *line, pipeline_metrics_t *metrics,
                            float *tone_strength, float *nn_high_clip_guard, int *nn_enabled, int *drc_mode,
                            int *params_dirty, int *enhancement_enabled, int *tone_enabled,
                            int *drc_strength, int *ldci_mode) {
    char response[2048];
    int n;
    int id = 0;
    int ok = 0;
    const char *err = NULL;

    fprintf(stderr, "[app_control] request from slot %d: %s\n", idx, line);

    /* Extract id */
    {
        const char *p = strstr(line, "\"id\":");
        if (p != NULL) {
            id = atoi(p + 5);
        }
    }

    if (strstr(line, "\"op\":\"status\"") != NULL) {
        n = snprintf(response, sizeof(response),
                     "{\"id\":%d,\"ok\":true,\"pipeline\":{"
                     "\"state\":\"%s\","
                     "\"capture_mode\":\"%s\","
                     "\"target_fps\":%u,"
                     "\"display_fps\":%.2f,"
                     "\"stream_drops\":%llu,"
                     "\"timeouts\":%llu,"
                     "\"transient_errors\":%llu,"
                     "\"fatal_errors\":%llu"
                     "},"
                     "\"processing\":{"
                     "\"nn_enabled\":%s,"
                     "\"nn_degraded\":%s,"
                     "\"tone_strength\":%.2f,"
                     "\"high_clip_guard\":%.1f,"
                     "\"infer_p95_ms\":%.2f,"
                     "\"transaction_p95_ms\":%.2f"
                     "},"
                     "\"outputs\":{"
                     "\"hdmi\":%s,"
                     "\"rtsp\":%s"
                     "}}\n",
                     id,
                     pipeline_state_name(metrics->state),
                     "linear", /* simplified — capture_mode from metrics */
                     metrics->display_fps > 0 ? 30u : 0u,
                     metrics->display_fps,
                     (unsigned long long) metrics->stream_drops,
                     (unsigned long long) metrics->frame_timeouts,
                     (unsigned long long) metrics->transient_errors,
                     (unsigned long long) metrics->fatal_errors,
                     *nn_enabled ? "true" : "false",
                     metrics->degraded_events > 0 ? "true" : "false",
                     (double) *tone_strength,
                     (double) *nn_high_clip_guard,
                     metrics->infer_p95_ms,
                     metrics->transaction_p95_ms,
                     metrics->display_frames > 0 ? "true" : "false",
                     metrics->stream_frames > 0 ? "true" : "false");
        ok = (n > 0 && (size_t) n < sizeof(response));
    } else if (strstr(line, "\"op\":\"set\"") != NULL) {
        /* Parse simple numeric params */
        const char *p;
        double v;

        p = strstr(line, "\"tone_strength\":");
        if (p != NULL) {
            v = atof(p + 16);
            if (v >= 0.0 && v <= 1.0) {
                *tone_strength = (float) v;
            }
        }
        p = strstr(line, "\"nn_high_clip_guard\":");
        if (p != NULL) {
            v = atof(p + 21);
            if (v >= 0.0 && v <= 100.0) {
                *nn_high_clip_guard = (float) v;
            }
        }
        p = strstr(line, "\"nn_clut_enabled\":");
        if (p != NULL) {
            if (strstr(p, "true") != NULL) {
                *nn_enabled = 1;
            } else if (strstr(p, "false") != NULL) {
                *nn_enabled = 0;
            }
        }
        p = strstr(line, "\"drc_mode\":");
        if (p != NULL) {
            if (strstr(p, "\"off\"") != NULL) {
                *drc_mode = 0;
            } else if (strstr(p, "\"auto\"") != NULL) {
                *drc_mode = 1;
            } else if (strstr(p, "\"manual\"") != NULL) {
                *drc_mode = 2;
            }
        }
        p = strstr(line, "\"drc_strength\":");
        if (p != NULL) { v = atof(p + 15); if (v >= 0 && v <= 1023) *drc_strength = (int)v; }
        p = strstr(line, "\"ldci_mode\":");
        if (p != NULL) {
            if (strstr(p, "\"off\"") != NULL) *ldci_mode = 0;
            else if (strstr(p, "\"auto\"") != NULL) *ldci_mode = 1;
        }
        p = strstr(line, "\"enhancement_enabled\":");
        if (p != NULL) {
            if (strstr(p, "true") != NULL) *enhancement_enabled = 1;
            else if (strstr(p, "false") != NULL) *enhancement_enabled = 0;
        }
        p = strstr(line, "\"tone_enabled\":");
        if (p != NULL) {
            if (strstr(p, "true") != NULL) *tone_enabled = 1;
            else if (strstr(p, "false") != NULL) *tone_enabled = 0;
        }

        n = snprintf(response, sizeof(response),
                     "{\"id\":%d,\"ok\":true,\"applied\":{"
                     "\"tone_strength\":%.2f,"
                     "\"high_clip_guard\":%.1f,"
                     "\"nn_clut_enabled\":%s,"
                     "\"drc_mode\":%d"
                     "}}\n",
                     id, (double) *tone_strength, (double) *nn_high_clip_guard,
                     *nn_enabled ? "true" : "false", *drc_mode);
        ok = (n > 0 && (size_t) n < sizeof(response));
        if (ok && params_dirty != NULL) { *params_dirty = 1; }
    } else {
        err = "unknown op";
    }

    if (err != NULL) {
        n = snprintf(response, sizeof(response), "{\"id\":%d,\"ok\":false,\"error\":\"%s\"}\n", id, err);
    }
    if (ok && n > 0 && (size_t) n < sizeof(response)) {
        ssize_t sent = send(s->client_fds[idx], response, (size_t) n, MSG_NOSIGNAL);
        if (sent < 0) {
            fprintf(stderr, "[app_control] send error: %d\n", errno);
        } else {
            fprintf(stderr, "[app_control] sent %zd bytes response\n", sent);
        }
    }
}

static int poll_count = 0;

void app_control_sock_poll(app_control_sock_t *s, pipeline_metrics_t *metrics,
                           float *tone_strength, float *nn_high_clip_guard,
                           int *nn_enabled, int *drc_mode, int *params_dirty,
                           int *enhancement_enabled, int *tone_enabled,
                           int *drc_strength, int *ldci_mode) {
    int fd;

    if (s == NULL) {
        return;
    }

    poll_count++;
    if (poll_count <= 5 || poll_count % 50 == 0) {
        fprintf(stderr, "[app_control] poll #%d\n", poll_count);
    }

    /* Accept new clients */
    fd = accept(s->listen_fd, NULL, NULL);
    if (fd >= 0) {
        fprintf(stderr, "[app_control] ACCEPT fd=%d\n", fd);
        set_nonblock(fd);
        for (int i = 0; i < SOCK_MAX_CLIENTS; i++) {
            if (s->client_fds[i] < 0) {
                s->client_fds[i] = fd;
                s->client_len[i] = 0;
                fprintf(stderr, "[app_control] client slot %d connected\n", i);
                fd = -1;
                break;
            }
        }
        if (fd >= 0) {
            fprintf(stderr, "[app_control] no free slot, closing\n");
            close(fd);
        }
    }

    /* Read and process each client */
    for (int i = 0; i < SOCK_MAX_CLIENTS; i++) {
        if (s->client_fds[i] < 0) {
            continue;
        }
        for (;;) {
            char chunk[1024];
            ssize_t n = recv(s->client_fds[i], chunk, sizeof(chunk), MSG_DONTWAIT);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                close_client(s, i);
                break;
            }
            for (ssize_t j = 0; j < n; j++) {
                int rc = append_char(s, i, chunk[j]);
                if (rc < 0) {
                    close_client(s, i);
                    goto next_client;
                }
                if (rc > 0) {
                    process_request(s, i, s->client_bufs[i], metrics, tone_strength,
                                    nn_high_clip_guard, nn_enabled, drc_mode, params_dirty,
                                    enhancement_enabled, tone_enabled, drc_strength, ldci_mode);
                    /* Request-response-close: simple and reliable */
                    close_client(s, i);
                    goto next_client;
                }
            }
        }
        next_client:;
    }
}

void app_control_sock_stop(app_control_sock_t *s) {
    if (s == NULL) {
        return;
    }
    for (int i = 0; i < SOCK_MAX_CLIENTS; i++) {
        close_client(s, i);
    }
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
    }
    free(s);
    LOG_INFO("app-control stopped");
}
