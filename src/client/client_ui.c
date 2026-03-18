/* Restored original content with fixes */
#include "client_ui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "../common/socket_utils.h"
#include "../common/protocol.h"
#include "../common/strutils.h"

// ANSI color codes
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_RESET   "\033[0m"
#define COLOR_BOLD    "\033[1m"

static int read_and_print_response(int fd) {
    char line[PROTO_MAX_LINE];
    ssize_t n = su_recv_line(fd, line, sizeof(line));
    if (n <= 0) return -1;
    if (strncmp(line, "OK", 2) == 0) {
        printf(COLOR_GREEN "✓ Success" COLOR_RESET "\n");
        while (1) {
            ssize_t m = su_recv_line(fd, line, sizeof(line));
            if (m < 0) return -1; // allow empty lines (m==0)
            if (strcmp(line, ".") == 0) break;
            printf("%s\n", line);
            fflush(stdout);
        }
        return 0;
    } else if (strncmp(line, "ERR", 3) == 0) {
        fprintf(stderr, "%s\n", line);
        fprintf(stderr, COLOR_RED "✗ Error: %s" COLOR_RESET "\n", line + 4);
        return 1;
    } else {
        printf("%s\n", line);
        return 0;
    }
}

static int reconnect_nm(const char *host, unsigned short port, const char *username, int *pfd) {
    if (*pfd >= 0) su_close(*pfd);
    for (int i = 0; i < 10; ++i) {
        int fd = su_connect(host, port);
        if (fd >= 0) {
            char reg[256]; snprintf(reg, sizeof(reg), "REGISTER %s", username);
            if (su_send_line(fd, reg) >= 0) {
                char line[PROTO_MAX_LINE];
                ssize_t n = su_recv_line(fd, line, sizeof(line));
                if (n > 0 && strncmp(line, "OK", 2) == 0) {
                    *pfd = fd;
                    printf(COLOR_GREEN "[info] Reconnected to NM." COLOR_RESET "\n"); fflush(stdout);
                    return 0;
                }
            }
            su_close(fd);
        }
        usleep(500000);
    }
    fprintf(stderr, COLOR_RED "[error] Unable to reconnect to NM after retries." COLOR_RESET "\n");
    return -1;
}

int run_client(const char *nm_host, unsigned short nm_port) {
    int fd = su_connect(nm_host, nm_port);
    if (fd < 0) {
        fprintf(stderr, COLOR_RED "✗ Failed to connect to NM" COLOR_RESET "\n");
        perror("connect NM");
        return 1;
    }

    char username[128];
    printf(COLOR_BLUE COLOR_BOLD "Welcome to Distributed File System" COLOR_RESET "\n");
    printf("Enter username: "); fflush(stdout);
    if (!fgets(username, sizeof(username), stdin)) return 1;
    str_trim(username);

    char reg[256]; snprintf(reg, sizeof(reg), "REGISTER %s", username);
    su_send_line(fd, reg);
    char line[PROTO_MAX_LINE];
    su_recv_line(fd, line, sizeof(line));
    if (strncmp(line, "OK", 2) == 0) {
        printf(COLOR_GREEN "✓ Successfully logged in as %s" COLOR_RESET "\n", username);
    } else {
        fprintf(stderr, COLOR_RED "✗ Login failed: %s" COLOR_RESET "\n", line + 4);
        return 1;
    }

    printf(COLOR_BLUE "\nAvailable commands:" COLOR_RESET "\n");
    printf("  VIEW, CREATE, READ, WRITE, UNDO, INFO, DELETE, STREAM, LIST\n");
    printf("  ADDACCESS, REMACCESS, EXEC, USERS, QUIT\n");
    printf(COLOR_BLUE "\nEnhanced terminal with colors for better readability!" COLOR_RESET "\n\n");

    while (1) {
        printf(COLOR_BLUE "> " COLOR_RESET); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        str_trim(line);
        if (!*line) continue;
        if (str_equals_ci(line, "QUIT")) break;

        if (str_startswith(line, "WRITE ")) {
            if (su_send_line(fd, line) < 0) {
                if (reconnect_nm(nm_host, nm_port, username, &fd) == 0) continue; else break;
            }
            ssize_t n = su_recv_line(fd, line, sizeof(line));
            if (n <= 0) {
                fprintf(stderr, COLOR_RED "✗ Error: Connection lost during WRITE setup" COLOR_RESET "\n");
                if (reconnect_nm(nm_host, nm_port, username, &fd)==0) continue; else break;
            }
            if (strncmp(line, "ERR", 3) == 0) {
                fprintf(stderr, COLOR_RED "✗ Write Error: %s" COLOR_RESET "\n", line + 4);
                continue;
            }
            if (strncmp(line, "OK", 2) != 0) {
                fprintf(stderr, COLOR_RED "✗ Error: Unexpected response: %s" COLOR_RESET "\n", line);
                if (reconnect_nm(nm_host, nm_port, username, &fd)==0) continue; else break;
            }
            printf(COLOR_BLUE "Enter edits as '<word_index> <text>' per line. End with ETIRW." COLOR_RESET "\n");
            while (1) {
                printf(COLOR_BLUE "write> " COLOR_RESET); fflush(stdout);
                if (!fgets(line, sizeof(line), stdin)) break;
                str_trim(line);
                if (su_send_line(fd, line) < 0) {
                    fprintf(stderr, COLOR_RED "✗ Error: Connection lost during WRITE" COLOR_RESET "\n");
                    if (reconnect_nm(nm_host, nm_port, username, &fd)==0) break; else { fd=-1; break; }
                }
                if (strcmp(line, "ETIRW") == 0) break;
            }
            if (fd >= 0) {
                if (read_and_print_response(fd) < 0) {
                    if (reconnect_nm(nm_host, nm_port, username, &fd) != 0) break;
                }
            } else {
                break;
            }
            continue;
        }

        char cmd_copy[PROTO_MAX_LINE]; snprintf(cmd_copy, sizeof(cmd_copy), "%s", line);
        char *space = strchr(cmd_copy, ' ');
        size_t cmd_len = space ? (size_t)(space - cmd_copy) : strlen(cmd_copy);
        for (size_t i=0;i<cmd_len;i++) cmd_copy[i] = (char)toupper((unsigned char)cmd_copy[i]);
        if (space) {
            char rebuilt[PROTO_MAX_LINE]; snprintf(rebuilt, sizeof(rebuilt), "%.*s%s", (int)cmd_len, cmd_copy, space);
            snprintf(line, sizeof(line), "%s", rebuilt);
        } else {
            snprintf(line, sizeof(line), "%s", cmd_copy);
        }
        if (su_send_line(fd, line) < 0) {
            if (reconnect_nm(nm_host, nm_port, username, &fd) == 0) continue; else break;
        }

        char first_line[PROTO_MAX_LINE];
        ssize_t r0 = su_recv_line(fd, first_line, sizeof(first_line));
        if (r0 <= 0) { if (reconnect_nm(nm_host, nm_port, username, &fd)==0) continue; else break; }
        if (strncmp(first_line, "ERR", 3) == 0) {
            fprintf(stderr, "%s\n", first_line);
            fprintf(stderr, COLOR_RED "✗ Error: %s" COLOR_RESET "\n", first_line+4);
            continue;
        }
        if (strncmp(first_line, "OK", 2) != 0) {
            printf("%s\n", first_line);
            continue;
        }

        char first_data[PROTO_MAX_LINE];
        ssize_t rn0 = su_recv_line(fd, first_data, sizeof(first_data));
        if (rn0 < 0) {
            // Connection glitch while reading payload header; try to reconnect and retry loop
            if (reconnect_nm(nm_host, nm_port, username, &fd)==0) continue; else break;
        }
        // Note: rn0 can be 0 for an empty first data line; handle below in non-direct flow
        int handled_direct = 0;
        if (strcmp(first_data, ".") != 0 && str_startswith(first_data, "CONNECT ")) {
            char host[256]; unsigned short ss_port = 0; char fname[256]; char sidx[64]="";
            int parts = sscanf(first_data, "CONNECT %255s %hu %255s %63s", host, &ss_port, fname, sidx);
            (void)parts; // suppress unused warning if not needed
            // Drain the remainder of NM's response (should be just a '.') to avoid protocol misalignment
            while (1) {
                char drain[PROTO_MAX_LINE];
                ssize_t dr = su_recv_line(fd, drain, sizeof(drain));
                if (dr <= 0) break;
                if (strcmp(drain, ".") == 0) break;
            }
            int ss_fd = su_connect(host, ss_port);
            if (ss_fd < 0) { fprintf(stderr, COLOR_RED "✗ Error: Cannot connect storage server" COLOR_RESET "\n"); handled_direct=1; continue; }
            if (str_startswith(line, "READ ")) {
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fname);
                su_send_line(ss_fd, req);
                read_and_print_response(ss_fd);
                su_close(ss_fd);
                char refresh[PROTO_MAX_LINE]; snprintf(refresh, sizeof(refresh), "REFRESH %s READ %s", fname, username);
                su_send_line(fd, refresh); read_and_print_response(fd);
                handled_direct=1; continue;
            } else if (str_startswith(line, "STREAM ")) {
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "STREAM %s", fname);
                su_send_line(ss_fd, req);
                char lbuf[PROTO_MAX_LINE]; ssize_t lr = su_recv_line(ss_fd, lbuf, sizeof(lbuf));
                if (lr>0 && strncmp(lbuf, "OK", 2)==0) {
                    while (1) {
                        lr = su_recv_line(ss_fd, lbuf, sizeof(lbuf));
                        if (lr<=0) { fprintf(stderr, COLOR_RED "✗ Stream aborted" COLOR_RESET "\n"); break; }
                        if (strcmp(lbuf, ".")==0) break;
                        printf("%s\n", lbuf); fflush(stdout);
                    }
                }
                su_close(ss_fd);
                char refresh[PROTO_MAX_LINE]; snprintf(refresh, sizeof(refresh), "REFRESH %s STREAM %s", fname, username);
                su_send_line(fd, refresh); read_and_print_response(fd);
                handled_direct=1; continue;
            } else if (str_startswith(line, "WRITE ")) {
                int sentence_index = atoi(sidx);
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "WRITE %s %d %s", fname, sentence_index, username);
                su_send_line(ss_fd, req);
                char wresp[PROTO_MAX_LINE]; ssize_t wn = su_recv_line(ss_fd, wresp, sizeof(wresp));
                if (wn>0 && strncmp(wresp, "OK", 2)==0) {
                    printf(COLOR_BLUE "Enter edits as '<word_index> <text>' per line. End with ETIRW." COLOR_RESET "\n");
                    while (1) {
                        printf(COLOR_BLUE "write> " COLOR_RESET); fflush(stdout);
                        if (!fgets(wresp, sizeof(wresp), stdin)) break;
                        str_trim(wresp);
                        su_send_line(ss_fd, wresp);
                        if (strcmp(wresp, "ETIRW") == 0) break;
                    }
                    ssize_t fs = su_recv_line(ss_fd, wresp, sizeof(wresp));
                    if (fs>0 && strncmp(wresp, "OK", 2)==0) {
                        printf(COLOR_GREEN "✓ Write successful" COLOR_RESET "\n");
                    } else if (fs>0 && strncmp(wresp, "ERR", 3)==0) {
                        fprintf(stderr, COLOR_RED "✗ Write Error: %s" COLOR_RESET "\n", wresp+4);
                    } else {
                        fprintf(stderr, COLOR_RED "✗ Write failed" COLOR_RESET "\n");
                    }
                    su_close(ss_fd);
                    char refresh[PROTO_MAX_LINE]; snprintf(refresh, sizeof(refresh), "REFRESH %s WRITE %s %d", fname, username, sentence_index);
                    su_send_line(fd, refresh); read_and_print_response(fd);
                } else {
                    fprintf(stderr, COLOR_RED "✗ Unable to start write session" COLOR_RESET "\n");
                    su_close(ss_fd);
                }
                handled_direct=1; continue;
            } else if (str_startswith(line, "UNDO ")) {
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "UNDO %s", fname);
                su_send_line(ss_fd, req);
                int rc = read_and_print_response(ss_fd);
                su_close(ss_fd);
                if (rc == 0) {
                    char refresh[PROTO_MAX_LINE]; snprintf(refresh, sizeof(refresh), "REFRESH %s UNDO %s", fname, username);
                    su_send_line(fd, refresh); read_and_print_response(fd);
                }
                handled_direct=1; continue;
            }
        }
        if (!handled_direct) {
            if (strcmp(first_data, ".") == 0) {
                // Empty payload: no lines to read
                continue;
            }
            printf("%s\n", first_data);
            while (1) {
                char more[PROTO_MAX_LINE];
                ssize_t rm = su_recv_line(fd, more, sizeof(more));
                if (rm < 0) break; // allow empty lines
                if (strcmp(more, ".") == 0) break;
                printf("%s\n", more);
            }
        }
    }

    printf(COLOR_BLUE "Goodbye!" COLOR_RESET "\n");
    su_close(fd);
    return 0;
}
