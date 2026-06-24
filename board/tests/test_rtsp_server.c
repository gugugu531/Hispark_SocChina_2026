#define _POSIX_C_SOURCE 200809L

#include "rtsp_server.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TEST_PORT 18554

static int connect_local(void) {
    struct sockaddr_in6 addr = {0};
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = htons(TEST_PORT);
    return connect(fd, (struct sockaddr*) &addr, sizeof(addr)) == 0 ? fd : -1;
}

static int transact(int fd, const char* request, const char* expected) {
    char response[2048] = {0};
    if (send(fd, request, strlen(request), 0) < 0) {
        return -1;
    }
    struct timespec wait = {0, 20000000};
    nanosleep(&wait, NULL);
    if (recv(fd, response, sizeof(response) - 1, 0) <= 0) {
        return -1;
    }
    return strstr(response, expected) == NULL ? -1 : 0;
}

int main(void) {
    rtsp_server_cfg_t cfg = {
        .port        = TEST_PORT,
        .fps         = 30,
        .stream_path = "live",
    };
    static const unsigned char h264_idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84};
    struct timespec wait = {0, 30000000};
    unsigned char rtp[64] = {0};
    int fd = -1;

    if (rtsp_server_start(&cfg) != 0) {
        return 1;
    }
    nanosleep(&wait, NULL);
    fd = connect_local();
    if (fd < 0) {
        goto fail;
    }
    if (transact(fd, "OPTIONS rtsp://[::1]:18554/live RTSP/1.0\r\nCSeq: 1\r\n\r\n", "Public: OPTIONS") != 0 ||
        transact(fd,
                 "DESCRIBE rtsp://[::1]:18554/live RTSP/1.0\r\n"
                 "CSeq: 2\r\nAccept: application/sdp\r\n\r\n",
                 "a=rtpmap:96 H264/90000") != 0 ||
        transact(fd,
                 "SETUP rtsp://[::1]:18554/live/trackID=0 RTSP/1.0\r\n"
                 "CSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n",
                 "interleaved=0-1") != 0 ||
        transact(fd,
                 "PLAY rtsp://[::1]:18554/live RTSP/1.0\r\n"
                 "CSeq: 4\r\nSession: 534f4343\r\n\r\n",
                 "RTP-Info:") != 0) {
        goto fail;
    }
    if (rtsp_server_publish_h264(h264_idr, sizeof(h264_idr)) != 0 ||
        recv(fd, rtp, sizeof(rtp), 0) < 4 + 12 + 3 || rtp[0] != '$' || rtp[1] != 0 || (rtp[4] >> 6) != 2 ||
        (rtp[5] & 0x7f) != 96 || rtp[16] != 0x65) {
        goto fail;
    }
    close(fd);
    fd = -1;
    nanosleep(&wait, NULL);
    fd = connect_local();
    if (fd < 0 ||
        transact(fd, "OPTIONS rtsp://[::1]:18554/live RTSP/1.0\r\nCSeq: 5\r\n\r\n", "RTSP/1.0 200 OK") != 0) {
        goto fail;
    }
    close(fd);
    rtsp_server_stop();
    puts("rtsp server tests passed");
    return 0;

fail:
    if (fd >= 0) {
        close(fd);
    }
    rtsp_server_stop();
    return 1;
}
