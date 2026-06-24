#define _POSIX_C_SOURCE 200809L

#include "rtsp_server.h"

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RTSP_REQUEST_CAP 8192u
#define RTSP_PATH_CAP    128u
#define RTP_PAYLOAD_MAX  1400u
#define RTP_PT_H264      96u

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int running;
    int stop;
    int listen_fd;
    int client_fd;
    int playing;
    unsigned port;
    unsigned fps;
    char path[RTSP_PATH_CAP];
    char request[RTSP_REQUEST_CAP + 1];
    size_t request_len;
    uint16_t rtp_seq;
    uint32_t rtp_timestamp;
    uint32_t rtp_ssrc;
    rtsp_idr_callback_t request_idr;
    void* request_idr_opaque;
} rtsp_state_t;

static rtsp_state_t g_rtsp = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
    .client_fd = -1,
};

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void close_client_locked(void) {
    if (g_rtsp.client_fd >= 0) {
        close(g_rtsp.client_fd);
    }
    g_rtsp.client_fd = -1;
    g_rtsp.playing = 0;
    g_rtsp.request_len = 0;
}

static int send_nonblock_locked(const void* data, size_t len) {
    const uint8_t* p = data;
    size_t sent = 0;
    unsigned waits = 0;

    while (sent < len) {
        ssize_t n = send(g_rtsp.client_fd, p + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits++ < 4) {
            struct pollfd pfd = {g_rtsp.client_fd, POLLOUT, 0};
            if (poll(&pfd, 1, 5) > 0 && (pfd.revents & POLLOUT)) {
                continue;
            }
        }
        LOG_WARN("rtsp client send failed after %zu/%zu bytes: errno=%d", sent, len, errno);
        close_client_locked();
        return -1;
    }
    return 0;
}

static int parse_cseq(const char* request) {
    const char* p = strstr(request, "\r\nCSeq:");
    if (p == NULL) {
        p = strstr(request, "\r\nCseq:");
    }
    return p == NULL ? 0 : atoi(p + 7);
}

static int send_response_locked(int cseq, const char* extra, const char* body) {
    char response[2048];
    size_t body_len = body == NULL ? 0 : strlen(body);
    int n;

    if (body != NULL) {
        char length_line[64];
        snprintf(length_line, sizeof(length_line), "Content-Length: %zu\r\n", body_len);
        n = snprintf(response, sizeof(response),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Server: socchina-rtsp/1.0\r\n"
                     "%s%s"
                     "\r\n%s",
                     cseq, extra == NULL ? "" : extra, length_line, body);
    } else {
        n = snprintf(response, sizeof(response),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Server: socchina-rtsp/1.0\r\n"
                     "%s\r\n",
                     cseq, extra == NULL ? "" : extra);
    }
    if (n < 0 || (size_t) n >= sizeof(response)) {
        return -1;
    }
    return send_nonblock_locked(response, (size_t) n);
}

static int send_error_locked(int cseq, int code, const char* reason) {
    char response[256];
    int n = snprintf(response, sizeof(response), "RTSP/1.0 %d %s\r\nCSeq: %d\r\n\r\n", code, reason, cseq);
    if (n < 0 || (size_t) n >= sizeof(response)) {
        return -1;
    }
    return send_nonblock_locked(response, (size_t) n);
}

static int request_targets_stream(const char* request) {
    char expected[RTSP_PATH_CAP + 2];
    snprintf(expected, sizeof(expected), "/%s", g_rtsp.path);
    return strstr(request, expected) != NULL;
}

static void process_request(const char* request) {
    char method[24] = {0};
    char sdp[512];
    char extra[512];
    int cseq = parse_cseq(request);
    int request_idr = 0;

    if (sscanf(request, "%23s", method) != 1) {
        return;
    }

    pthread_mutex_lock(&g_rtsp.lock);
    if (strcmp(method, "OPTIONS") == 0) {
        (void) send_response_locked(
            cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", NULL);
    } else if (strcmp(method, "DESCRIBE") == 0 && request_targets_stream(request)) {
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- 0 0 IN IP6 ::\r\n"
                 "s=SocChina H264 Stream\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "c=IN IP6 ::\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=1\r\n"
                 "a=control:trackID=0\r\n");
        snprintf(extra, sizeof(extra), "Content-Type: application/sdp\r\n");
        (void) send_response_locked(cseq, extra, sdp);
    } else if (strcmp(method, "SETUP") == 0 && request_targets_stream(request)) {
        if (strstr(request, "RTP/AVP/TCP") == NULL) {
            (void) send_error_locked(cseq, 461, "Unsupported Transport");
        } else {
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                     "Session: 534f4343\r\n");
            (void) send_response_locked(cseq, extra, NULL);
        }
    } else if (strcmp(method, "PLAY") == 0 && request_targets_stream(request)) {
        g_rtsp.playing = 1;
        snprintf(extra, sizeof(extra),
                 "Session: 534f4343\r\n"
                 "RTP-Info: url=rtsp://0.0.0.0:%u/%s/trackID=0;seq=%u;rtptime=%u\r\n",
                 g_rtsp.port, g_rtsp.path, g_rtsp.rtp_seq, g_rtsp.rtp_timestamp);
        (void) send_response_locked(cseq, extra, NULL);
        request_idr = 1;
    } else if (strcmp(method, "GET_PARAMETER") == 0) {
        (void) send_response_locked(cseq, "Session: 534f4343\r\n", NULL);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        (void) send_response_locked(cseq, "Session: 534f4343\r\n", NULL);
        close_client_locked();
    } else if (!request_targets_stream(request)) {
        (void) send_error_locked(cseq, 404, "Not Found");
    } else {
        (void) send_error_locked(cseq, 405, "Method Not Allowed");
    }
    pthread_mutex_unlock(&g_rtsp.lock);

    if (request_idr && g_rtsp.request_idr != NULL) {
        g_rtsp.request_idr(g_rtsp.request_idr_opaque);
    }
}

static void consume_client_data(void) {
    for (;;) {
        ssize_t n;
        size_t consumed = 0;

        pthread_mutex_lock(&g_rtsp.lock);
        if (g_rtsp.client_fd < 0 || g_rtsp.request_len == RTSP_REQUEST_CAP) {
            pthread_mutex_unlock(&g_rtsp.lock);
            return;
        }
        n = recv(g_rtsp.client_fd, g_rtsp.request + g_rtsp.request_len, RTSP_REQUEST_CAP - g_rtsp.request_len,
                 MSG_DONTWAIT);
        if (n > 0) {
            g_rtsp.request_len += (size_t) n;
            g_rtsp.request[g_rtsp.request_len] = '\0';
        } else if (n == 0) {
            close_client_locked();
            pthread_mutex_unlock(&g_rtsp.lock);
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_client_locked();
            pthread_mutex_unlock(&g_rtsp.lock);
            return;
        }

        while (g_rtsp.request_len - consumed >= 4) {
            if ((uint8_t) g_rtsp.request[consumed] == '$') {
                size_t interleaved_len =
                    ((uint8_t) g_rtsp.request[consumed + 2] << 8) | (uint8_t) g_rtsp.request[consumed + 3];
                if (g_rtsp.request_len - consumed < interleaved_len + 4) {
                    break;
                }
                consumed += interleaved_len + 4;
                continue;
            }
            char* end = strstr(g_rtsp.request + consumed, "\r\n\r\n");
            if (end == NULL) {
                break;
            }
            size_t request_len = (size_t) (end - (g_rtsp.request + consumed)) + 4;
            char one[RTSP_REQUEST_CAP];
            if (request_len >= sizeof(one)) {
                close_client_locked();
                pthread_mutex_unlock(&g_rtsp.lock);
                return;
            }
            memcpy(one, g_rtsp.request + consumed, request_len);
            one[request_len] = '\0';
            consumed += request_len;
            pthread_mutex_unlock(&g_rtsp.lock);
            process_request(one);
            pthread_mutex_lock(&g_rtsp.lock);
            if (g_rtsp.client_fd < 0) {
                pthread_mutex_unlock(&g_rtsp.lock);
                return;
            }
        }
        if (consumed > 0 && consumed <= g_rtsp.request_len) {
            memmove(g_rtsp.request, g_rtsp.request + consumed, g_rtsp.request_len - consumed);
            g_rtsp.request_len -= consumed;
        }
        if (g_rtsp.request_len == RTSP_REQUEST_CAP) {
            close_client_locked();
        }
        pthread_mutex_unlock(&g_rtsp.lock);

        if (n <= 0) {
            return;
        }
    }
}

static void accept_client(void) {
    int fd = accept(g_rtsp.listen_fd, NULL, NULL);
    int send_buffer = 1024 * 1024;
    if (fd < 0) {
        return;
    }
    (void) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
    if (set_nonblock(fd) != 0) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&g_rtsp.lock);
    close_client_locked();
    g_rtsp.client_fd = fd;
    pthread_mutex_unlock(&g_rtsp.lock);
    LOG_INFO("rtsp client connected");
}

static void* server_thread(void* arg) {
    (void) arg;
    while (!g_rtsp.stop) {
        struct pollfd fds[2];
        nfds_t nfds = 1;

        fds[0].fd = g_rtsp.listen_fd;
        fds[0].events = POLLIN;
        pthread_mutex_lock(&g_rtsp.lock);
        if (g_rtsp.client_fd >= 0) {
            fds[1].fd = g_rtsp.client_fd;
            fds[1].events = POLLIN | POLLERR | POLLHUP;
            nfds = 2;
        }
        pthread_mutex_unlock(&g_rtsp.lock);

        if (poll(fds, nfds, 100) <= 0) {
            continue;
        }
        if (fds[0].revents & POLLIN) {
            accept_client();
        }
        if (nfds == 2 && (fds[1].revents & POLLIN)) {
            consume_client_data();
        } else if (nfds == 2 && (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            pthread_mutex_lock(&g_rtsp.lock);
            close_client_locked();
            pthread_mutex_unlock(&g_rtsp.lock);
        }
    }
    return NULL;
}

static const uint8_t* find_start_code(const uint8_t* p, const uint8_t* end, size_t* code_len) {
    while (p + 3 <= end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            *code_len = 3;
            return p;
        }
        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            *code_len = 4;
            return p;
        }
        p++;
    }
    return NULL;
}

static int send_rtp_packet_locked(const uint8_t* payload, size_t payload_len, int marker) {
    uint8_t packet[4 + 12 + RTP_PAYLOAD_MAX];
    size_t rtp_len = 12 + payload_len;

    packet[0] = '$';
    packet[1] = 0;
    packet[2] = (uint8_t) (rtp_len >> 8);
    packet[3] = (uint8_t) rtp_len;
    packet[4] = 0x80;
    packet[5] = (uint8_t) (RTP_PT_H264 | (marker ? 0x80 : 0));
    packet[6] = (uint8_t) (g_rtsp.rtp_seq >> 8);
    packet[7] = (uint8_t) g_rtsp.rtp_seq++;
    packet[8] = (uint8_t) (g_rtsp.rtp_timestamp >> 24);
    packet[9] = (uint8_t) (g_rtsp.rtp_timestamp >> 16);
    packet[10] = (uint8_t) (g_rtsp.rtp_timestamp >> 8);
    packet[11] = (uint8_t) g_rtsp.rtp_timestamp;
    packet[12] = (uint8_t) (g_rtsp.rtp_ssrc >> 24);
    packet[13] = (uint8_t) (g_rtsp.rtp_ssrc >> 16);
    packet[14] = (uint8_t) (g_rtsp.rtp_ssrc >> 8);
    packet[15] = (uint8_t) g_rtsp.rtp_ssrc;
    memcpy(packet + 16, payload, payload_len);
    return send_nonblock_locked(packet, 4 + rtp_len);
}

static int send_nalu_locked(const uint8_t* nalu, size_t len, int last_nalu) {
    if (len <= RTP_PAYLOAD_MAX) {
        return send_rtp_packet_locked(nalu, len, last_nalu);
    }

    uint8_t fu[RTP_PAYLOAD_MAX];
    uint8_t nal_header = nalu[0];
    size_t offset = 1;
    int first = 1;

    fu[0] = (uint8_t) ((nal_header & 0xe0) | 28);
    while (offset < len) {
        size_t chunk = len - offset;
        int last;
        if (chunk > RTP_PAYLOAD_MAX - 2) {
            chunk = RTP_PAYLOAD_MAX - 2;
        }
        last = offset + chunk == len;
        fu[1] = (uint8_t) ((nal_header & 0x1f) | (first ? 0x80 : 0) | (last ? 0x40 : 0));
        memcpy(fu + 2, nalu + offset, chunk);
        if (send_rtp_packet_locked(fu, chunk + 2, last && last_nalu) != 0) {
            return -1;
        }
        first = 0;
        offset += chunk;
    }
    return 0;
}

int rtsp_server_start(const rtsp_server_cfg_t* cfg) {
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    struct sockaddr* sa;
    socklen_t sa_len;
    int reuse = 1;
    int family;
    int v6only = 0;

    if (cfg == NULL || cfg->port == 0 || cfg->fps == 0 || cfg->stream_path == NULL ||
        cfg->stream_path[0] == '\0' || strcmp(cfg->stream_path, "/") == 0 ||
        strlen(cfg->stream_path) >= sizeof(g_rtsp.path)) {
        return -1;
    }
    if (g_rtsp.running) {
        return 0;
    }

    /* 决定绑定地址：指定地址绑定到特定接口，否则绑定所有接口 */
    if (cfg->bind_addr != NULL && cfg->bind_addr[0] != '\0' &&
        strcmp(cfg->bind_addr, "127.0.0.1") == 0) {
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr4.sin_port = htons((uint16_t) cfg->port);
        family = AF_INET;
        sa = (struct sockaddr*) &addr4;
        sa_len = sizeof(addr4);
    } else if (cfg->bind_addr != NULL && cfg->bind_addr[0] != '\0') {
        /* 通用 IPv4/IPv6 地址 */
        if (inet_pton(AF_INET, cfg->bind_addr, &addr4.sin_addr) == 1) {
            memset(&addr4, 0, sizeof(addr4));
            addr4.sin_family = AF_INET;
            addr4.sin_port = htons((uint16_t) cfg->port);
            family = AF_INET;
            sa = (struct sockaddr*) &addr4;
            sa_len = sizeof(addr4);
        } else if (inet_pton(AF_INET6, cfg->bind_addr, &addr6.sin6_addr) == 1) {
            memset(&addr6, 0, sizeof(addr6));
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port = htons((uint16_t) cfg->port);
            family = AF_INET6;
            sa = (struct sockaddr*) &addr6;
            sa_len = sizeof(addr6);
            v6only = 1;
        } else {
            LOG_ERR("rtsp_server: cannot parse bind_addr '%s'", cfg->bind_addr);
            return -1;
        }
    } else {
        /* 默认：绑定所有接口 */
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons((uint16_t) cfg->port);
        family = AF_INET6;
        sa = (struct sockaddr*) &addr6;
        sa_len = sizeof(addr6);
    }

    g_rtsp.listen_fd = socket(family, SOCK_STREAM, 0);
    if (g_rtsp.listen_fd < 0) {
        return -1;
    }
    (void) setsockopt(g_rtsp.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (family == AF_INET6) {
        (void) setsockopt(g_rtsp.listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
    if (bind(g_rtsp.listen_fd, sa, sa_len) != 0 ||
        listen(g_rtsp.listen_fd, 2) != 0 || set_nonblock(g_rtsp.listen_fd) != 0) {
        close(g_rtsp.listen_fd);
        g_rtsp.listen_fd = -1;
        return -1;
    }

    g_rtsp.port = cfg->port;
    g_rtsp.fps = cfg->fps;
    strcpy(g_rtsp.path, cfg->stream_path[0] == '/' ? cfg->stream_path + 1 : cfg->stream_path);
    g_rtsp.request_idr = cfg->request_idr;
    g_rtsp.request_idr_opaque = cfg->request_idr_opaque;
    g_rtsp.rtp_seq = 1;
    g_rtsp.rtp_timestamp = 0;
    g_rtsp.rtp_ssrc = 0x534f4343u;
    g_rtsp.stop = 0;
    if (pthread_create(&g_rtsp.thread, NULL, server_thread, NULL) != 0) {
        close(g_rtsp.listen_fd);
        g_rtsp.listen_fd = -1;
        return -1;
    }
    g_rtsp.running = 1;
    if (cfg->bind_addr != NULL && cfg->bind_addr[0] != '\0') {
        LOG_INFO("rtsp listening on %s:%u/%s (RTP over TCP)", cfg->bind_addr, g_rtsp.port, g_rtsp.path);
    } else {
        LOG_INFO("rtsp listening on [::]:%u/%s (RTP over TCP)", g_rtsp.port, g_rtsp.path);
    }
    return 0;
}

int rtsp_server_publish_h264(const uint8_t* annexb, size_t len) {
    const uint8_t* end;
    const uint8_t* start;
    size_t code_len = 0;

    if (annexb == NULL || len == 0 || !g_rtsp.running) {
        return -1;
    }
    pthread_mutex_lock(&g_rtsp.lock);
    if (g_rtsp.client_fd < 0 || !g_rtsp.playing) {
        pthread_mutex_unlock(&g_rtsp.lock);
        return 0;
    }

    end = annexb + len;
    start = find_start_code(annexb, end, &code_len);
    if (start == NULL) {
        (void) send_nalu_locked(annexb, len, 1);
    } else {
        while (start != NULL) {
            const uint8_t* nalu = start + code_len;
            size_t next_code_len = 0;
            const uint8_t* next = find_start_code(nalu, end, &next_code_len);
            const uint8_t* nalu_end = next == NULL ? end : next;
            while (nalu_end > nalu && nalu_end[-1] == 0) {
                nalu_end--;
            }
            if (nalu_end > nalu && send_nalu_locked(nalu, (size_t) (nalu_end - nalu), next == NULL) != 0) {
                break;
            }
            start = next;
            code_len = next_code_len;
        }
    }
    g_rtsp.rtp_timestamp += 90000u / g_rtsp.fps;
    pthread_mutex_unlock(&g_rtsp.lock);
    return 0;
}

int rtsp_server_stop(void) {
    if (!g_rtsp.running) {
        return 0;
    }
    g_rtsp.stop = 1;
    pthread_join(g_rtsp.thread, NULL);
    pthread_mutex_lock(&g_rtsp.lock);
    close_client_locked();
    if (g_rtsp.listen_fd >= 0) {
        close(g_rtsp.listen_fd);
        g_rtsp.listen_fd = -1;
    }
    g_rtsp.running = 0;
    pthread_mutex_unlock(&g_rtsp.lock);
    LOG_INFO("rtsp stopped");
    return 0;
}
