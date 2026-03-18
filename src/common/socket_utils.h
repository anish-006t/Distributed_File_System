#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <stddef.h>
#include <netinet/in.h>

int su_listen(uint16_t port, int backlog);
int su_accept(int server_fd, char *peer_ip, size_t ip_len, uint16_t *peer_port);
int su_connect(const char *host, uint16_t port);

ssize_t su_send_all(int fd, const void *buf, size_t len);
ssize_t su_recv_n(int fd, void *buf, size_t len);

// line I/O (\n terminated, without including the trailing \n in out buffer)
// Returns number of bytes in out (excluding NUL), or -1 on error, -2 on EOF
ssize_t su_recv_line(int fd, char *out, size_t maxlen);
ssize_t su_send_line(int fd, const char *line);

void su_close(int fd);

#endif // SOCKET_UTILS_H
