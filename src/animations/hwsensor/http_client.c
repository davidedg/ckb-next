#include "http_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define INITIAL_BUFFER_SIZE (4 * 1024)
#define MAX_RESPONSE_SIZE   (4 * 1024 * 1024) /* sanity cap on one GET's memory use, not a data plausibility filter */

static int connect_with_timeout(unsigned short port, int timeout_ms){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1){
        close(fd);
        return -1;
    }

    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if(rc == 0){
        fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if(errno != EINPROGRESS){
        close(fd);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if(rc <= 0){
        close(fd);
        return -1;
    }

    int err = 0;
    socklen_t errlen = sizeof(err);
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0){
        close(fd);
        return -1;
    }

    fcntl(fd, F_SETFL, flags);
    return fd;
}

static int send_all(int fd, const char* data, size_t len){
    size_t sent = 0;
    while(sent < len){
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if(n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static const char* find_header_end(const char* buf, size_t len){
    if(len < 4)
        return NULL;
    for(size_t i = 0; i + 4 <= len; i++){
        if(buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    }
    return NULL;
}

int http_get_loopback(unsigned short port, const char* path, int connect_timeout_ms, int recv_timeout_ms, http_response* out){
    out->body = NULL;
    out->body_len = 0;
    out->status_code = 0;

    int fd = connect_with_timeout(port, connect_timeout_ms);
    if(fd < 0)
        return -1;

    struct timeval rtv;
    rtv.tv_sec = recv_timeout_ms / 1000;
    rtv.tv_usec = (recv_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    char request[1024];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: 127.0.0.1:%u\r\nConnection: close\r\n\r\n",
        path, (unsigned)port);
    if(req_len <= 0 || (size_t)req_len >= sizeof(request) || send_all(fd, request, (size_t)req_len) != 0){
        close(fd);
        return -2;
    }

    size_t capacity = INITIAL_BUFFER_SIZE;
    size_t len = 0;
    char* buf = malloc(capacity);
    if(!buf){
        close(fd);
        return -3;
    }

    for(;;){
        if(len == capacity){
            if(capacity >= MAX_RESPONSE_SIZE){
                free(buf);
                close(fd);
                return -4;
            }
            size_t new_capacity = capacity * 2;
            char* grown = realloc(buf, new_capacity);
            if(!grown){
                free(buf);
                close(fd);
                return -3;
            }
            buf = grown;
            capacity = new_capacity;
        }
        ssize_t n = recv(fd, buf + len, capacity - len, 0);
        if(n < 0){
            free(buf);
            close(fd);
            return -5; /* read error or timeout */
        }
        if(n == 0)
            break; /* clean EOF -- HTTP/1.0 + Connection: close means the peer is done */
        len += (size_t)n;
    }
    close(fd);

    const char* header_end = find_header_end(buf, len);
    if(!header_end){
        free(buf);
        return -6;
    }

    long status = 0;
    sscanf(buf, "HTTP/%*d.%*d %ld", &status);

    size_t header_len = (size_t)(header_end - buf) + 4;
    size_t body_len = len - header_len;
    char* body = malloc(body_len + 1);
    if(!body){
        free(buf);
        return -3;
    }
    memcpy(body, buf + header_len, body_len);
    body[body_len] = '\0';
    free(buf);

    out->body = body;
    out->body_len = body_len;
    out->status_code = status;
    return 0;
}

void http_response_free(http_response* out){
    free(out->body);
    out->body = NULL;
    out->body_len = 0;
}
