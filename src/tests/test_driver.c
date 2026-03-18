#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../common/socket_utils.h"
#include "../common/protocol.h"
#include "../common/strutils.h"

static int expect_ok_block(int fd, char *buf, size_t bufsz) {
    ssize_t n = su_recv_line(fd, buf, bufsz);
    if (n <= 0) return -1;
    if (strncmp(buf, "OK", 2) != 0) return 1;
    // read until '.'
    while (1) {
        n = su_recv_line(fd, buf, bufsz);
        if (n <= 0) return -1;
        if (strcmp(buf, ".") == 0) break;
        // accumulate if needed
    }
    return 0;
}

static int expect_ok_collect(int fd, char **out) {
    char line[PROTO_MAX_LINE];
    ssize_t n = su_recv_line(fd, line, sizeof(line));
    if (n <= 0) return -1;
    if (strncmp(line, "OK", 2) != 0) return 1;
    size_t cap = 4096, len = 0; char *acc = malloc(cap);
    while (1) {
        ssize_t m = su_recv_line(fd, line, sizeof(line));
        if (m <= 0) { free(acc); return -1; }
        if (strcmp(line, ".") == 0) break;
        size_t bl = strlen(line);
        if (len + bl + 2 > cap) { cap*=2; acc = realloc(acc, cap); }
        memcpy(acc+len, line, bl); len+=bl; acc[len++]='\n';
    }
    acc[len] = '\0';
    *out = acc;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <nm_host> <nm_port>\n", argv[0]);
        return 2;
    }
    const char *host = argv[1];
    unsigned short port = (unsigned short)atoi(argv[2]);
    int fd = su_connect(host, port);
    if (fd < 0) { perror("connect NM"); return 2; }

    // Register
    su_send_line(fd, "REGISTER tester");
    char line[PROTO_MAX_LINE]; su_recv_line(fd, line, sizeof(line));

    // Create and read back empty
    su_send_line(fd, "CREATE mouse.txt");
    char *blk = NULL; int rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "CREATE failed\n"); return 1; } free(blk);

    // Read empty file
    su_send_line(fd, "READ mouse.txt");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "READ empty failed\n"); return 1; } free(blk);

    // Write sentence 0
    su_send_line(fd, "WRITE mouse.txt 0");
    su_recv_line(fd, line, sizeof(line)); // expect OK
    su_send_line(fd, "1 Im just a mouse.");
    su_send_line(fd, "ETIRW");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "WRITE S0 failed\n"); return 1; } free(blk);

    // Append sentence 1
    su_send_line(fd, "WRITE mouse.txt 1");
    su_recv_line(fd, line, sizeof(line)); // OK
    su_send_line(fd, "1 I dont like PNS");
    su_send_line(fd, "ETIRW");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "WRITE S1 failed\n"); return 1; } free(blk);

    // Read and check substring
    su_send_line(fd, "READ mouse.txt");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "READ after write failed\n"); return 1; }
    if (!strstr(blk, "Im just a mouse.")) { fprintf(stderr, "Missing sentence 0 content\n"); return 1; }
    if (!strstr(blk, "I dont like PNS")) { fprintf(stderr, "Missing sentence 1 content\n"); return 1; }
    free(blk);

    // STREAM sanity (just ensure OK and some tokens)
    su_send_line(fd, "STREAM mouse.txt");
    su_recv_line(fd, line, sizeof(line)); // OK
    int count = 0;
    while (1) {
        ssize_t m = su_recv_line(fd, line, sizeof(line));
        if (m <= 0) break;
        if (strcmp(line, ".") == 0) break;
        count++;
        if (count > 10) break; // don't block too long
    }

    // INFO
    su_send_line(fd, "INFO mouse.txt");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "INFO failed\n"); return 1; }
    if (!strstr(blk, "Owner:")) { fprintf(stderr, "INFO missing Owner\n"); return 1; }
    free(blk);

    // UNDO
    su_send_line(fd, "UNDO mouse.txt");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "UNDO failed\n"); return 1; } free(blk);

    // LIST
    su_send_line(fd, "LIST");
    rc = expect_ok_block(fd, line, sizeof(line)); if (rc != 0) { fprintf(stderr, "LIST failed\n"); return 1; }

    // VIEW -l
    su_send_line(fd, "VIEW -l");
    rc = expect_ok_collect(fd, &blk); if (rc != 0) { fprintf(stderr, "VIEW -l failed\n"); return 1; } free(blk);

    su_close(fd);
    printf("All tests passed.\n");
    return 0;
}
