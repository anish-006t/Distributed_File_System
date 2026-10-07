#include "socket_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>

int su_listen(uint16_t port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int su_accept(int server_fd, char *peer_ip, size_t ip_len, uint16_t *peer_port) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = accept(server_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) return -1;
    if (peer_ip && ip_len > 0) {
        const char *p = inet_ntop(AF_INET, &addr.sin_addr, peer_ip, ip_len);
        if (!p && ip_len > 0) peer_ip[0] = '\0';
    }
    if (peer_port) *peer_port = ntohs(addr.sin_port);
    // Line protocol sends many small writes; disable Nagle to avoid ~40ms delayed-ACK stalls
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

int su_connect(const char *host, uint16_t port) {
    // getaddrinfo is thread-safe (gethostbyname returns a shared static buffer
    // and crashed the NM when many client threads connected concurrently)
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return fd;
}

ssize_t su_send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char*)buf;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

ssize_t su_recv_n(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char*)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return (ssize_t)total; // EOF
        total += (size_t)n;
    }
    return (ssize_t)total;
}

ssize_t su_recv_line(int fd, char *out, size_t maxlen) {
    size_t i = 0;
    while (i + 1 < maxlen) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -2; // EOF
        if (c == '\n') break;
        out[i++] = c;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

ssize_t su_send_line(int fd, const char *line) {
    size_t len = strlen(line);
    if (su_send_all(fd, line, len) < 0) return -1;
    return su_send_all(fd, "\n", 1);
}

void su_close(int fd) {
    if (fd >= 0) close(fd);
}
