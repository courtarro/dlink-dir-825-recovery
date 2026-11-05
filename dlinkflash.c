/* dlinkflash - Tool flash older d-link routers via emergency web interface
 *
 * You must have 192.168.0.x/24 (where X=2-254) IP address on the
 * interface connected to the d-link and use the emergency flashing interface
 *
 * Copyright 2014 Daniel Dickinson, 2026 Ethan Trewhitt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#define ROUTER_IP        "192.168.0.1"
#define CHUNK_SIZE       512
#define GET_RECV_TIMEOUT 5   /* seconds of silence before we assume the page is complete */
#define POST_RECV_TIMEOUT 60 /* seconds to wait for the router's reply after upload */

static void die_oom(void) {
    fprintf(stderr, "Out of memory\n");
    exit(2);
}

/* Append datalen bytes to a growable buffer. *post must be NULL iff *post_len == 0. */
static void build_post_bin(uint8_t **post, size_t *post_len, const uint8_t *newdata, size_t datalen) {
    uint8_t *newpost = realloc(*post, *post_len + datalen);
    if (!newpost)
        die_oom();
    memcpy(newpost + *post_len, newdata, datalen);
    *post_len += datalen;
    *post = newpost;
}

static void build_post(uint8_t **post, size_t *post_len, const char *newchar, size_t *content_len) {
    size_t n = strlen(newchar);
    build_post_bin(post, post_len, (const uint8_t *)newchar, n);
    if (content_len)
        *content_len += n;
}

static void usage(const char *exename) {
    printf("Usage: %s <filename> [-d]\n", exename);
    printf("   Interface attached to d-link must have IP address 192.168.0.x/24 where x != 1\n");
    exit(1);
}

/* Read a whole file into memory. Returns malloc'd buffer, sets *outlen. Exits on error. */
static uint8_t *read_file(const char *path, size_t *outlen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }

    uint8_t *data = NULL;
    size_t len = 0, cap = 0;

    for (;;) {
        if (cap - len < 4096) {
            size_t newcap = cap ? cap * 2 : 65536;
            uint8_t *tmp = realloc(data, newcap);
            if (!tmp)
                die_oom();
            data = tmp;
            cap = newcap;
        }
        ssize_t n = read(fd, data + len, cap - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror(path);
            close(fd);
            free(data);
            exit(2);
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);

    *outlen = len;
    return data;
}

static int open_socket(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    int tcpflush = 1;
    int bufsz = CHUNK_SIZE;
    int smallwindow = CHUNK_SIZE;
    int mss = 1024;

    /* These must be set before connect() to take effect. */
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &tcpflush, sizeof(tcpflush));
    setsockopt(sock, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
    setsockopt(sock, IPPROTO_TCP, TCP_WINDOW_CLAMP, &smallwindow, sizeof(smallwindow));

    struct sockaddr_in ipaddr;
    memset(&ipaddr, 0, sizeof(ipaddr));
    ipaddr.sin_family = AF_INET;
    ipaddr.sin_port = htons(80);
    ipaddr.sin_addr.s_addr = inet_addr(ROUTER_IP);
    if (connect(sock, (struct sockaddr *)&ipaddr, sizeof(ipaddr)) < 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }
    return sock;
}

static void set_recv_timeout(int sock, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* Send buf in CHUNK_SIZE pieces. If delay_us > 0, sleep after each full chunk.
 * Returns 0 on success, -1 on error (errno set). */
static int send_all(int sock, const uint8_t *buf, size_t len, const char *label,
                    useconds_t delay_us, int debug) {
    size_t curpos = 0;

    while (curpos < len) {
        size_t want = len - curpos;
        int full = 0;
        if (want >= CHUNK_SIZE) {
            want = CHUNK_SIZE;
            full = 1;
        }

        ssize_t sent = send(sock, buf + curpos, want, 0);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (debug)
            fprintf(stderr, "Sent %zd bytes\n", sent);

        curpos += (size_t)sent;
        printf("\r%zu/%zu Bytes written: %s %g%% complete                    ",
               curpos, len, label, ((double)curpos / (double)len) * 100.0);
        fflush(stdout);

        if (full && delay_us)
            usleep(delay_us); /* slow the transfer enough for the router to cope */
    }
    return 0;
}

int main(int argc, char *argv[]) {
    uint8_t *firmware = NULL;
    uint8_t *post = NULL;
    size_t postlen = 0;
    uint8_t *get = NULL;
    size_t getlen = 0;
    uint8_t *content = NULL;
    size_t contentlen = 0;
    size_t nonnllen = 0;
    size_t firmwarelen = 0;
    char contentlenstr[64];
    int debug = 0;

    if (argc < 2)
        usage(argv[0]);

    if (argc == 3) {
        if (!strncmp(argv[2], "-d", 2))
            debug = 1;
        else
            usage(argv[0]);
    } else if (argc > 3) {
        usage(argv[0]);
    }

    printf("Load firmware file %s\n", argv[1]);
    firmware = read_file(argv[1], &firmwarelen);
    if (firmwarelen == 0) {
        fprintf(stderr, "%s: file is empty\n", argv[1]);
        free(firmware);
        exit(2);
    }
    printf("Firmware %zu bytes long\n", firmwarelen);

    build_post(&content, &contentlen, "---------------------------7de1fe13304\r\n", NULL);
    nonnllen += 2;
    build_post(&content, &contentlen, "Content-Disposition: form-data; name=\"files\"; filename=\"C:\\My Documents\\firmware.bin\"\r\n", &nonnllen);
    build_post(&content, &contentlen, "Content-Type: application/octet-stream\r\n", &nonnllen);
    build_post(&content, &contentlen, "\r\n", &nonnllen);
    build_post_bin(&content, &contentlen, firmware, firmwarelen);
    build_post(&content, &contentlen, "\r\n---------------------------7de1fe13304--\r\n", NULL);
    nonnllen += 4;

    /* IE6 off-by-one content-length error? */
    snprintf(contentlenstr, sizeof(contentlenstr), "%zu\r\n", nonnllen + firmwarelen + 1);

    build_post(&get, &getlen, "GET / HTTP/1.1\r\n", NULL);
    build_post(&get, &getlen, "Accept: image/gif, image/x-xbitmap, image/jpeg, image/pjpeg, */*\r\n", NULL);
    build_post(&get, &getlen, "Accept-Language: en-ca\r\n", NULL);
    build_post(&get, &getlen, "Accept-Encoding: gzip, deflate\r\n", NULL);
    build_post(&get, &getlen, "User-Agent: Mozilla/4.0 (compatible; MSIE 6.0; Windows 98; .NET CLR 1.1.4322)\r\n", NULL);
    build_post(&get, &getlen, "Host: 192.168.0.1\r\n", NULL);
    build_post(&get, &getlen, "Connection: Keep-Alive\r\n", NULL);
    build_post(&get, &getlen, "\r\n", NULL);

    build_post(&post, &postlen, "POST /cgi/index HTTP/1.1\r\n", NULL);
    build_post(&post, &postlen, "Accept: image/gif, image/x-xbitmap, image/jpeg, image/pjpeg, */*\r\n", NULL);
    build_post(&post, &postlen, "Referer: http://192.168.0.1\r\n", NULL);
    build_post(&post, &postlen, "Accept-Language: en-ca\r\n", NULL);
    build_post(&post, &postlen, "Content-Type: multipart/form-data; boundary=---------------------------7de1fe13304\r\n", NULL);
    build_post(&post, &postlen, "Accept-Encoding: gzip, deflate\r\n", NULL);
    build_post(&post, &postlen, "User-Agent: Mozilla/4.0 (compatible; MSIE 6.0; Windows 98; .NET CLR 1.1.4322)\r\n", NULL);
    build_post(&post, &postlen, "Host: 192.168.0.1\r\n", NULL);
    build_post(&post, &postlen, "Content-Length: ", NULL);
    build_post(&post, &postlen, contentlenstr, NULL);
    build_post(&post, &postlen, "Connection: Keep-Alive\r\n", NULL);
    build_post(&post, &postlen, "Cache-Control: no-cache\r\n", NULL);
    build_post(&post, &postlen, "\r\n", NULL);
    build_post_bin(&post, &postlen, content, contentlen);
    free(content);
    free(firmware);

    /* ---- Phase 1: GET so the router is in a state to receive the POST ---- */
    printf("Initiating GET so that router is in state to receive POST....\n");
    fflush(stdout);

    int sock = open_socket();
    if (sock < 0) {
        perror("connect");
        free(get);
        free(post);
        exit(7);
    }
    if (send_all(sock, get, getlen, "GET", 0, debug) < 0) {
        perror("send_get");
        close(sock);
        free(get);
        free(post);
        exit(7);
    }
    printf("\nFinished sending GET. Waiting for response.\n");
    free(get);
    get = NULL;

    /* Read the page until the router closes the connection or goes quiet. */
    set_recv_timeout(sock, GET_RECV_TIMEOUT);
    char recvbuf[1024];
    for (;;) {
        ssize_t recvlen = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
        if (recvlen < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* timed out: assume page is complete */
            perror("recv");
            close(sock);
            free(post);
            exit(8);
        }
        if (recvlen == 0)
            break;
        recvbuf[recvlen] = 0;
        if (debug)
            fprintf(stderr, "Got %zd bytes data\n%s", recvlen, recvbuf);
    }
    printf("Got page from which upload is done.\n");
    shutdown(sock, SHUT_RDWR);
    close(sock);

    /* ---- Phase 2: POST the firmware ---- */
    printf("Initiating transfer....\n");
    fflush(stdout);
    sock = open_socket();
    if (sock < 0) {
        perror("connect");
        free(post);
        exit(7);
    }
    if (send_all(sock, post, postlen, "Upload", 5000, debug) < 0) {
        perror("send");
        close(sock);
        free(post);
        exit(5);
    }
    printf("\nFinished sending post. Waiting for response.\n");
    free(post);
    post = NULL;

    /* Accumulate the reply (with a sliding window) and look for "count_down". */
    set_recv_timeout(sock, POST_RECV_TIMEOUT);
    char resp[4096];
    size_t resplen = 0;
    int success = 0;

    for (;;) {
        ssize_t n = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                fprintf(stderr, "Timed out waiting for response from device.\n");
                break;
            }
            perror("recv");
            close(sock);
            exit(6);
        }
        if (n == 0)
            break;
        if (debug)
            fprintf(stderr, "Got %zd bytes\n", n);

        if (resplen + (size_t)n > sizeof(resp) - 1) {
            size_t drop = resplen + (size_t)n - (sizeof(resp) - 1);
            memmove(resp, resp + drop, resplen - drop);
            resplen -= drop;
        }
        memcpy(resp + resplen, recvbuf, (size_t)n);
        resplen += (size_t)n;
        resp[resplen] = 0;

        if (strstr(resp, "count_down")) {
            success = 1;
            break;
        }
    }

    shutdown(sock, SHUT_RDWR);
    close(sock);

    if (success) {
        printf("Firmware successfully sent.  Please wait for device to reboot.\n");
        return 0;
    }

    printf("Error sending firmware to device.  Response is:\n%s\n", resp);
    return 6;
}
