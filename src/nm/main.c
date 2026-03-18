#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>

#include "../common/socket_utils.h"
#include "../common/protocol.h"
#include "../common/strutils.h"
#include "../common/errors.h"
#include "../common/log.h"
#include "nm_state.h"
#include "exec_utils.h"

#define BACKLOG 128

typedef struct {
    int fd;
    char peer_ip[64];
    uint16_t peer_port;
} ConnInfo;

static NMState *G_ST = NULL;
static int G_RR_CURSOR = 0; // simple round-robin cursor for server selection

// Logging helpers: now only write to log file (no terminal echo) per user request
static void nm_info(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_info("%s", buf);
}
static void nm_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[1024]; vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_error("%s", buf);
}

static void format_time(time_t t, char *buf, size_t n) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

// Structured log helper: timestamp | component | opcode | user | file | replicas | status | duration_ms
static void nm_log_event(const char *component, const char *opcode, const char *user, const char *file, const char *replicas, const char *status, double duration_ms) {
    char ts[64];
    format_time(time(NULL), ts, sizeof(ts));
    char repbuf[512];
    snprintf(repbuf, sizeof(repbuf), "%s", replicas ? replicas : "-");
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s | %s | %s | %s | %s | %s | %s | %.2fms",
             ts, component ? component : "NM", opcode ? opcode : "-", user ? user : "-", file ? file : "-", repbuf, status ? status : "-", duration_ms);
    log_info("%s", buf);
}

static void update_stats_from_text(FileMeta *fm, const char *text, const char *accessor) {
    if (!fm || !text) return;
    size_t chars = strlen(text);
    size_t words = 0;
    int in_word = 0;
    for (const char *p = text; *p; ++p) {
        if (*p > ' ' && *p <= '~') { // ASCII non-space
            if (!in_word) { in_word = 1; words++; }
        } else {
            in_word = 0;
        }
    }
    fm->chars = chars;
    fm->words = words;
    fm->last_access = time(NULL);
    free(fm->last_accessor);
    fm->last_accessor = accessor ? strdup(accessor) : NULL;
}

// Helper to update last_modified timestamp (called after WRITE/UNDO operations)
static void update_last_modified(FileMeta *fm) {
    if (!fm) return;
    fm->modified = time(NULL);
}

static int ss_send_simple(const char *host, uint16_t port, const char *line, char **out_text) {
    int fd = su_connect(host, port);
    if (fd < 0) return -1;
    if (su_send_line(fd, line) < 0) { su_close(fd); return -1; }
    char buf[PROTO_MAX_LINE];
    ssize_t n = su_recv_line(fd, buf, sizeof(buf));
    if (n <= 0) { su_close(fd); return -1; }
    if (strncmp(buf, "OK", 2) == 0) {
        // Collect optional text block until '.'
        char *acc = NULL; size_t cap = 0, len = 0;
        while (1) {
            ssize_t m = su_recv_line(fd, buf, sizeof(buf));
            if (m <= 0) { break; }
            if (strcmp(buf, ".") == 0) break;
            size_t bl = strlen(buf);
            if (len + bl + 2 > cap) { cap = cap ? cap*2 : 4096; acc = (char*)realloc(acc, cap); }
            memcpy(acc + len, buf, bl); len += bl; acc[len++]='\n';
        }
        if (acc) acc[len] = '\0';
        if (out_text) *out_text = acc; else free(acc);
        su_close(fd);
        return 0;
    } else if (strncmp(buf, "ERR", 3) == 0) {
        su_close(fd);
        if (out_text) *out_text = strdup(buf);
        return -2;
    }
    su_close(fd);
    return -1;
}

// Helper functions for multiple storage servers
static int ss_send_to_all(FileMeta *fm, const char *request, char **out_text) {
    if (!fm || !fm->storage_servers) return -1;
    
    int success_count = 0;
    StorageServerRef *ssr = fm->storage_servers;
    
    while (ssr) {
        char *response = NULL;
        int rc = ss_send_simple(ssr->host, ssr->port, request, out_text ? &response : NULL);
        if (rc == 0) {
            success_count++;
            if (out_text && !*out_text && response) {
                *out_text = response; // Use first successful response
            } else if (response) {
                free(response);
            }
        }
        ssr = ssr->next;
    }
    
    return (success_count > 0) ? 0 : -1;
}

// Get PRIMARY storage server for writes (always first healthy server)
static StorageServerRef *get_primary_server(FileMeta *fm) {
    if (!fm || !fm->storage_servers) return NULL;
    pthread_mutex_lock(&G_ST->state_mutex);
    for (StorageServerRef *ssr = fm->storage_servers; ssr; ssr = ssr->next) {
        for (StorageServer *s = G_ST->ss_list; s; s = s->next) {
            if (s->id == ssr->id && s->healthy) {
                pthread_mutex_unlock(&G_ST->state_mutex);
                return ssr; // Return FIRST healthy server
            }
        }
    }
    pthread_mutex_unlock(&G_ST->state_mutex);
    // Fallback: return first server even if health unknown
    return fm->storage_servers;
}

static StorageServerRef *get_available_server(FileMeta *fm) {
    if (!fm || !fm->storage_servers) return NULL;
    // Collect healthy candidates
    StorageServerRef *candidates[16]; int n=0;
    pthread_mutex_lock(&G_ST->state_mutex);
    for (StorageServerRef *ssr = fm->storage_servers; ssr && n<16; ssr = ssr->next) {
        for (StorageServer *s = G_ST->ss_list; s; s = s->next) {
            if (s->id == ssr->id && s->healthy) { candidates[n++] = ssr; break; }
        }
    }
    pthread_mutex_unlock(&G_ST->state_mutex);
    if (n > 0) {
        int idx = (G_RR_CURSOR++ % n); if (idx < 0) idx = 0;
        return candidates[idx];
    }
    // Fallback: first reachable by connect test
    for (StorageServerRef *ssr = fm->storage_servers; ssr; ssr = ssr->next) {
        int test_fd = su_connect(ssr->host, ssr->port);
        if (test_fd >= 0) { su_close(test_fd); return ssr; }
    }
    return fm->storage_servers; // ultimate fallback
}
static void send_text_lines(int fd, const char *text) {
    // text may be NULL
    proto_send_text_block(fd, text ? text : "");
}

static void handle_client(int fd) {
    char username[128] = "";
    char line[PROTO_MAX_LINE];
    while (1) {
        ssize_t n = su_recv_line(fd, line, sizeof(line));
        if (n == -2 || n <= 0) break; // EOF or error
        str_trim(line);
        if (!*line) continue;
        char *tokens[8]; char tmp[PROTO_MAX_LINE];
        snprintf(tmp, sizeof(tmp), "%s", line);
        int tc = str_split_ws(tmp, tokens, 8);
    if (tc <= 0) { proto_send_err(fd, ERR_BAD_REQUEST, "Empty"); continue; }

    if (strcmp(tokens[0], "CREATE") == 0) {
            // Two-phase create: NM assigns replicas and returns CONNECT to a primary SS.
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            if (strlen(tokens[1]) > 255) { proto_send_err(fd, ERR_BAD_REQUEST, "Filename too long"); continue; }
            pthread_mutex_lock(&G_ST->state_mutex);
            if (nm_get_file(G_ST, tokens[1])) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "File exists"); continue; }
            // Create metadata entry and pick replicas but do not push content yet
            FileMeta *fm = nm_create_file(G_ST, tokens[1], username);
            if (!fm || !fm->storage_servers) {
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_err(fd, ERR_INTERNAL, "No storage servers available");
                continue;
            }
            // Choose primary server (round-robin selection)
            StorageServerRef *primary = get_available_server(fm);
            nm_info("CREATE provisioned %s owner=%s primary=%s:%u replicas=%p", fm->name, username, primary?primary->host:"-", primary?primary->port:0, fm->storage_servers);
            pthread_mutex_unlock(&G_ST->state_mutex);
            // Tell client to connect to the primary SS to perform the CREATE operation
            proto_send_ok(fd);
            char line_out[PROTO_MAX_LINE];
            snprintf(line_out, sizeof(line_out), "CONNECT %s %u %s", primary->host, primary->port, fm->name);
            su_send_line(fd, line_out);
            su_send_line(fd, ".");
            
        } else if (strcmp(tokens[0], "USERS") == 0) {
            proto_send_ok(fd);
            
            pthread_mutex_lock(&G_ST->state_mutex);
            
            // Display active users
            su_send_line(fd, "=== ACTIVE USERS ===");
            UserSession *active = nm_get_active_users(G_ST);
            if (!active) {
                su_send_line(fd, "No active users");
            } else {
                while (active) {
                    char time_str[64];
                    struct tm *tm_info = localtime(&active->login_time);
                    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                    char user_info[256];
                    snprintf(user_info, sizeof(user_info), "[ACTIVE] %s (logged in: %s)", 
                            active->username, time_str);
                    su_send_line(fd, user_info);
                    active = active->next;
                }
            }
            
            // Display all users who have ever logged in
            su_send_line(fd, "");
            su_send_line(fd, "=== ALL REGISTERED USERS ===");
            UserSession *all_users = nm_get_all_users(G_ST);
            if (!all_users) {
                su_send_line(fd, "No users registered");
            } else {
                while (all_users) {
                    char last_seen[64];
                    struct tm *tm_info = localtime(&all_users->last_activity);
                    strftime(last_seen, sizeof(last_seen), "%Y-%m-%d %H:%M:%S", tm_info);
                    char user_info[256];
                    snprintf(user_info, sizeof(user_info), "[%s] %s (last seen: %s)", 
                            all_users->is_active ? "ACTIVE" : "OFFLINE", 
                            all_users->username, last_seen);
                    su_send_line(fd, user_info);
                    all_users = all_users->next;
                }
            }
            
            pthread_mutex_unlock(&G_ST->state_mutex);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "VIEW") == 0) {
            int all = 0, detailed = 0;
            for (int i = 1; i < tc; ++i) {
                if (strcmp(tokens[i], "-a") == 0) { all = 1; continue; }
                if (strcmp(tokens[i], "-l") == 0) { detailed = 1; continue; }
                if (tokens[i][0] == '-') {
                    // Accept combined flags in any order, e.g., -al or -la
                    for (const char *p = tokens[i] + 1; *p; ++p) {
                        if (*p == 'a') all = 1;
                        else if (*p == 'l') detailed = 1;
                    }
                }
            }
            nm_info("VIEW by %s flags: -a=%d -l=%d", username, all, detailed);
            proto_send_ok(fd);
            if (detailed) {
                su_send_line(fd, "---------------------------------------------------------");
                su_send_line(fd, "| Filename   | Words | Chars | Last Access        | Owner |");
                su_send_line(fd, "---------------------------------------------------------");
            }
            for (size_t i = 0; i < G_ST->files->nbuckets; ++i) {
                HMEntry *e = G_ST->files->buckets[i];
                while (e) {
                    FileMeta *fm = (FileMeta*)e->value;
                    if (all || nm_user_has_read(fm, username)) {
                        if (detailed) {
                            char buf[512], ts[64];
                            format_time(fm->last_access, ts, sizeof(ts));
                            snprintf(buf, sizeof(buf), "| %-10s | %5zu | %5zu | %-18s | %-5s |", fm->name, fm->words, fm->chars, ts, fm->owner);
                            su_send_line(fd, buf);
                        } else su_send_line(fd, fm->name);
                    }
                    e = e->next;
                }
            }
            if (detailed) su_send_line(fd, "---------------------------------------------------------");
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "STATUS") == 0) {
            // STATUS - show storage servers health
            proto_send_ok(fd);
            pthread_mutex_lock(&G_ST->state_mutex);
            for (StorageServer *s = G_ST->ss_list; s; s = s->next) {
                char line[256];
                char t_ok[32]="-", t_ck[32]="-";
                if (s->last_ok) { time_t t=s->last_ok; struct tm tm; localtime_r(&t,&tm); strftime(t_ok,sizeof(t_ok),"%H:%M:%S", &tm); }
                if (s->last_check) { time_t t=s->last_check; struct tm tm; localtime_r(&t,&tm); strftime(t_ck,sizeof(t_ck),"%H:%M:%S", &tm); }
                snprintf(line, sizeof(line), "SS %d %s:%u %s fails=%d last_ok=%s last_ck=%s",
                         s->id, s->host, s->port, s->healthy?"UP":"DOWN", s->fail_count, t_ok, t_ck);
                su_send_line(fd, line);
            }
            pthread_mutex_unlock(&G_ST->state_mutex);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "CREATE") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            if (strlen(tokens[1]) > 255) { proto_send_err(fd, ERR_BAD_REQUEST, "Filename too long"); continue; }
            pthread_mutex_lock(&G_ST->state_mutex);
            if (nm_get_file(G_ST, tokens[1])) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "File exists"); continue; }
            StorageServer *ss = nm_pick_ss(G_ST);
            if (!ss) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
            char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "CREATE %s %s", tokens[1], username);
            
            // Create file metadata with multiple storage servers
            FileMeta *fm = nm_create_file(G_ST, tokens[1], username);
            if (!fm || !fm->storage_servers) {
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_err(fd, ERR_INTERNAL, "No storage servers available");
                continue;
            }
            
            // Send CREATE request to all selected storage servers
            int success_count = 0; int total_replicas = 0;
            StorageServerRef *ssr = fm->storage_servers;
            while (ssr) {
                total_replicas++;
                int rc = ss_send_simple(ssr->host, ssr->port, req, NULL);
                if (rc == 0) success_count++;
                ssr = ssr->next;
            }

            // Require at-least-two replicas to have the file, or all available if fewer than two
            int required = (total_replicas >= 2) ? 2 : total_replicas;
            if (required > 0 && success_count >= required) {
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, "File Created Successfully!");
            } else {
                // Cleanup - remove from NM state if creation did not meet replication threshold
                nm_delete_file(G_ST, tokens[1]);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_err(fd, ERR_INTERNAL, "Failed to create file on required replicas");
            }
        } else if (strcmp(tokens[0], "READ") == 0) {
            // Direct client ↔ SS flow: provide a storage server endpoint instead of proxying content.
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
            StorageServerRef *ssr = get_available_server(fm);
            if (!ssr) { proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
            proto_send_ok(fd);
            char line_out[256];
            snprintf(line_out, sizeof(line_out), "CONNECT %s %u %s", ssr->host, ssr->port, fm->name);
            su_send_line(fd, line_out);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "DELETE") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
            char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "DELETE %s", fm->name);
            // Require ALL replicas delete successfully
            int total = 0; int success = 0; int locked_fail = 0; int notfound_fail = 0;
            for (StorageServerRef *ssr = fm->storage_servers; ssr; ssr = ssr->next) {
                total++;
                char *errtxt = NULL;
                int rc = ss_send_simple(ssr->host, ssr->port, req, &errtxt);
                if (rc == 0) {
                    success++;
                } else if (errtxt) {
                    if (strstr(errtxt, "Locked")) locked_fail++;
                    else if (strstr(errtxt, "No file")) notfound_fail++;
                }
                if (errtxt) free(errtxt);
            }
            if (success == total) {
                pthread_mutex_lock(&G_ST->state_mutex);
                nm_delete_file(G_ST, fm->name);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, "File deleted from all replicas");
            } else if (locked_fail) {
                proto_send_err(fd, ERR_LOCKED, "One or more replicas locked");
            } else if (notfound_fail && success==0) {
                proto_send_err(fd, ERR_NOT_FOUND, "File missing on replicas");
            } else {
                proto_send_err(fd, ERR_CONSISTENCY, "Replica delete mismatch");
            }
        } else if (strcmp(tokens[0], "INFO") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            proto_send_ok(fd);
            // Format timestamps
            char buf[512]; char ts_created[64]; char ts_modified[64]; char ts_access[64];
            format_time(fm->created, ts_created, sizeof(ts_created));
            format_time(fm->modified, ts_modified, sizeof(ts_modified));
            format_time(fm->last_access, ts_access, sizeof(ts_access));
            snprintf(buf, sizeof(buf), "File: %s", fm->name); su_send_line(fd, buf);
            snprintf(buf, sizeof(buf), "Owner: %s", strcmp(fm->owner, "unknown") == 0 ? "(pending)" : fm->owner); su_send_line(fd, buf);
            snprintf(buf, sizeof(buf), "Created: %s", ts_created); su_send_line(fd, buf);
            snprintf(buf, sizeof(buf), "Last Modified: %s", ts_modified); su_send_line(fd, buf);
            // Provide legacy-compatible Size line for test scripts expecting 'Size: N'
            snprintf(buf, sizeof(buf), "Size: %zu", fm->chars); su_send_line(fd, buf);
            // Additional detailed stats
            snprintf(buf, sizeof(buf), "Words: %zu", fm->words); su_send_line(fd, buf);
            snprintf(buf, sizeof(buf), "Chars: %zu", fm->chars); su_send_line(fd, buf);
            if (fm->last_accessor) {
                snprintf(buf, sizeof(buf), "Last Accessed: %s by %s", ts_access, fm->last_accessor); su_send_line(fd, buf);
            }
            su_send_line(fd, "Access:");
            for (Access *a = fm->acl; a; a = a->next) {
                snprintf(buf, sizeof(buf), "  %s (%s)", a->user, a->can_write?"RW":"R"); su_send_line(fd, buf);
            }
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "ADDACCESS") == 0) {
            if (tc < 4) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: ADDACCESS -R|-W <file> <user>"); continue; }
            bool write = strcmp(tokens[1], "-W") == 0;
            if (!write && strcmp(tokens[1], "-R") != 0) { proto_send_err(fd, ERR_BAD_REQUEST, "Flag"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[2]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
            // Validate that target user exists
            if (!nm_user_exists(G_ST, tokens[3])) {
                proto_send_err(fd, ERR_NOT_FOUND, "Invalid user or user does not exist");
                continue;
            }
            pthread_mutex_lock(&G_ST->state_mutex);
            bool has_read = nm_user_has_read(fm, tokens[3]);
            bool has_write = nm_user_has_write(fm, tokens[3]);
            if (write) {
                if (has_write) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "User already has this access"); continue; }
                nm_acl_grant(fm, tokens[3], true);
                nm_state_save(G_ST);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, has_read ? "Access upgraded to write" : "Access granted successfully!");
            } else {
                if (has_read) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "User already has this access"); continue; }
                nm_acl_grant(fm, tokens[3], false);
                nm_state_save(G_ST);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, "Access granted successfully!");
            }
        } else if (strcmp(tokens[0], "REMACCESS") == 0) {
            if (tc < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: REMACCESS <file> <user>"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
            if (strcmp(fm->owner, tokens[2]) == 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner cannot remove their own access."); continue; }
            pthread_mutex_lock(&G_ST->state_mutex);
            if (!nm_user_has_read(fm, tokens[2])) { pthread_mutex_unlock(&G_ST->state_mutex); char emsg[256]; snprintf(emsg, sizeof(emsg), "User %s never had this access", tokens[2]); proto_send_err(fd, ERR_NOT_FOUND, emsg); continue; }
            nm_acl_revoke(fm, tokens[2]);
            nm_state_save(G_ST);
            pthread_mutex_unlock(&G_ST->state_mutex);
            proto_send_text_block(fd, "Access removed successfully!");
        } else if (strcmp(tokens[0], "WRITE") == 0) {
            // Provide connection info for direct WRITE session with centralized locking.
            if (tc < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: WRITE <file> <sentence_index>"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (!nm_user_has_write(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No write access"); continue; }
            
            // Centralized lock acquisition (NM-level) - single atomic check-and-lock
            int sentence_idx = atoi(tokens[2]);
            pthread_mutex_lock(&G_ST->state_mutex);
            if (!nm_lock_sentence(fm, sentence_idx, username)) {
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_err(fd, ERR_LOCKED, "Sentence locked by another user");
                continue;
            }
            pthread_mutex_unlock(&G_ST->state_mutex);
            
            StorageServerRef *ssr = get_primary_server(fm); // Always use primary for WRITE
            if (!ssr) {
                // Release lock if can't get server
                pthread_mutex_lock(&G_ST->state_mutex);
                nm_unlock_sentence(fm, sentence_idx, username);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_err(fd, ERR_INTERNAL, "No storage server");
                continue;
            }
            proto_send_ok(fd);
            char info[256];
            snprintf(info, sizeof(info), "CONNECT %s %u %s %s", ssr->host, ssr->port, fm->name, tokens[2]);
            su_send_line(fd, info);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "UNDO") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (!nm_user_has_write(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No write access"); continue; }
            StorageServerRef *ssr = get_primary_server(fm); // Always use primary for UNDO
            if (!ssr) { proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
            // Return endpoint for client to perform UNDO directly
            proto_send_ok(fd);
            char out[256]; snprintf(out, sizeof(out), "CONNECT %s %u %s", ssr->host, ssr->port, fm->name);
            su_send_line(fd, out);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "STREAM") == 0) {
            // Provide endpoint for direct streaming.
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
            StorageServerRef *ssr = get_available_server(fm);
            if (!ssr) { proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
            proto_send_ok(fd);
            char out[256]; snprintf(out, sizeof(out), "CONNECT %s %u %s", ssr->host, ssr->port, fm->name);
            su_send_line(fd, out);
            su_send_line(fd, ".");
        } else if (strcmp(tokens[0], "EXEC") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileMeta *fm = nm_get_file(G_ST, tokens[1]);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
            char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fm->name);
            char *text = NULL;
            int rc = ss_send_to_all(fm, req, &text);
            if (rc != 0) { proto_send_err(fd, ERR_INTERNAL, "SS error"); continue; }
            if (!text) text = strdup("");
            int exit_code = 0; char *out = exec_capture_bash(text, &exit_code);
            free(text);
            if (!out) { proto_send_err(fd, ERR_INTERNAL, "Exec failed"); continue; }
            proto_send_text_block(fd, out);
            free(out);
        } else if (strcmp(tokens[0], "REFRESH") == 0) {
            // REFRESH <file> <mode> <user> [sentence_idx]
            if (tc < 4) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: REFRESH <file> <mode> <user> [sentence_idx]"); continue; }
            const char *fname = tokens[1]; const char *mode = tokens[2]; const char *actor = tokens[3];
            int sentence_idx = (tc >= 5) ? atoi(tokens[4]) : -1;
            FileMeta *fm = nm_get_file(G_ST, fname);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            
            // Release NM-level lock if this is after a WRITE operation
            if (strcmp(mode, "WRITE") == 0 && sentence_idx >= 0) {
                pthread_mutex_lock(&G_ST->state_mutex);
                nm_unlock_sentence(fm, sentence_idx, actor);
                pthread_mutex_unlock(&G_ST->state_mutex);
            }
            // Pick a source server and open a READ stream
            StorageServerRef *source = get_available_server(fm);
            if (!source) { proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
            clock_t t0 = clock();
            int sfd = su_connect(source->host, source->port);
            if (sfd < 0) { proto_send_err(fd, ERR_INTERNAL, "Source connect failed"); continue; }
            char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fm->name);
            if (su_send_line(sfd, req) < 0) { su_close(sfd); proto_send_err(fd, ERR_INTERNAL, "Source send failed"); continue; }
            char r0[PROTO_MAX_LINE];
            if (su_recv_line(sfd, r0, sizeof(r0)) <= 0 || strncmp(r0, "OK", 2) != 0) {
                su_close(sfd);
                proto_send_err(fd, ERR_INTERNAL, "Source read failed");
                continue;
            }
            // Connect to destination replicas (excluding source) and send SYNC_FILE header
            typedef struct { int fd; const char *host; uint16_t port; } Dest;
            Dest dests[32]; int nd = 0;
            for (StorageServerRef *ssr = fm->storage_servers; ssr && nd < 32; ssr = ssr->next) {
                if (ssr == source) continue;
                int cfd = su_connect(ssr->host, ssr->port);
                if (cfd < 0) continue;
                char sync_cmd[PROTO_MAX_LINE]; snprintf(sync_cmd, sizeof(sync_cmd), "SYNC_FILE %s", fm->name);
                if (su_send_line(cfd, sync_cmd) < 0) { su_close(cfd); continue; }
                dests[nd].fd = cfd; dests[nd].host = ssr->host; dests[nd].port = ssr->port; nd++;
            }
            // Stream lines from source to all destinations, computing stats on the fly
            size_t total_chars = 0, total_words = 0; int in_word = 0;
            char linebuf[PROTO_MAX_LINE];
            while (1) {
                ssize_t rn = su_recv_line(sfd, linebuf, sizeof(linebuf));
                if (rn <= 0) { break; }
                if (strcmp(linebuf, ".") == 0) {
                    // finish: send '.' to all destinations and read their OK
                    for (int i = 0; i < nd; ++i) {
                        su_send_line(dests[i].fd, ".");
                        // Best-effort read of OK
                        char okb[PROTO_MAX_LINE];
                        if (su_recv_line(dests[i].fd, okb, sizeof(okb)) > 0) {
                            // consume trailing '.'
                            su_recv_line(dests[i].fd, okb, sizeof(okb));
                        }
                        su_close(dests[i].fd);
                    }
                    break;
                }
                // forward to all destinations
                for (int i = 0; i < nd; ++i) su_send_line(dests[i].fd, linebuf);
                // update stats counters (ASCII non-space definition matches update_stats_from_text)
                for (char *p = linebuf; *p; ++p) {
                    if (*p > ' ' && *p <= '~') { if (!in_word) { in_word = 1; total_words++; } }
                    else { in_word = 0; }
                    total_chars++;
                }
                // account for the newline we add on SS side during write
                total_chars++; in_word = 0; // newline acts as space
            }
            su_close(sfd);
            // Update metadata (owner assignment on first write preserved)
            pthread_mutex_lock(&G_ST->state_mutex);
            fm->chars = total_chars;
            fm->words = total_words;
            fm->last_access = time(NULL);
            free(fm->last_accessor); fm->last_accessor = strdup(actor);
            if ((strcmp(mode, "WRITE") == 0) || (strcmp(mode, "UNDO") == 0)) update_last_modified(fm);
            if (strcmp(fm->owner, "unknown") == 0 && strcmp(mode, "WRITE") == 0) {
                free(fm->owner); fm->owner = strdup(actor);
            }
            nm_state_save(G_ST);
            pthread_mutex_unlock(&G_ST->state_mutex);
            clock_t t1 = clock();
            double dur = ((double)(t1 - t0)) / CLOCKS_PER_SEC * 1000.0;
            char replicas_buf[512]; replicas_buf[0] = '\0';
            for (StorageServerRef *d = fm->storage_servers; d; d = d->next) {
                if (replicas_buf[0]) strncat(replicas_buf, ",", sizeof(replicas_buf)-strlen(replicas_buf)-1);
                char small[64]; snprintf(small, sizeof(small), "%s:%u", d->host, d->port);
                strncat(replicas_buf, small, sizeof(replicas_buf)-strlen(replicas_buf)-1);
            }
            nm_log_event("NM", "REFRESH", actor, fm->name, replicas_buf, "OK", dur);
            proto_send_text_block(fd, "Refresh OK");
        } else if (strcmp(tokens[0], "UNLOCK") == 0) {
            // UNLOCK <file> <sentence_idx> - release NM-level lock (used on client disconnect/error)
            if (tc < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: UNLOCK <file> <sentence_idx>"); continue; }
            const char *fname = tokens[1];
            int sentence_idx = atoi(tokens[2]);
            FileMeta *fm = nm_get_file(G_ST, fname);
            if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
            pthread_mutex_lock(&G_ST->state_mutex);
            nm_unlock_sentence(fm, sentence_idx, username);
            pthread_mutex_unlock(&G_ST->state_mutex);
            proto_send_ok(fd);
            su_send_line(fd, ".");
            continue;
        } else {
            proto_send_err(fd, ERR_BAD_REQUEST, "Unknown command");
        }
    }
    
    // Client disconnected - release all locks held by this user
    pthread_mutex_lock(&G_ST->state_mutex);
    for (size_t i = 0; i < G_ST->files->nbuckets; ++i) {
        HMEntry *e = G_ST->files->buckets[i];
        while (e) {
            FileMeta *fm = (FileMeta*)e->value;
            nm_unlock_all_sentences(fm, username);
            e = e->next;
        }
    }
    pthread_mutex_unlock(&G_ST->state_mutex);
    
    su_close(fd);
}

// Background thread: periodically check health of all storage servers
static void *health_thread(void *arg) {
    (void)arg;
    while (1) {
        // Build a local snapshot of servers to probe
        typedef struct { int id; char host[256]; uint16_t port; bool was_healthy; } Probe;
        Probe list[256]; int n = 0;
        pthread_mutex_lock(&G_ST->state_mutex);
        for (StorageServer *s = G_ST->ss_list; s && n < 256; s = s->next) {
            list[n].id = s->id; snprintf(list[n].host, sizeof(list[n].host), "%s", s->host); list[n].port = s->port; list[n].was_healthy = s->healthy; n++;
        }
        pthread_mutex_unlock(&G_ST->state_mutex);

        for (int i=0;i<n;i++) {
            int fd = su_connect(list[i].host, list[i].port);
            bool ok = (fd >= 0);
            if (fd >= 0) su_close(fd);
            // Update state
            pthread_mutex_lock(&G_ST->state_mutex);
            for (StorageServer *s = G_ST->ss_list; s; s = s->next) {
                if (s->id == list[i].id) {
                    s->last_check = time(NULL);
                    if (ok) {
                        s->healthy = true;
                        s->fail_count = 0;
                        s->last_ok = s->last_check;
                    } else {
                        s->fail_count += 1;
                        if (s->fail_count >= 3) s->healthy = false; // require 3 consecutive failures to mark down
                    }
                    bool changed = (s->healthy != list[i].was_healthy);
                    if (changed) {
                        nm_log_event("NM", "HEALTH", NULL, NULL, NULL, s->healthy ? "UP" : "DOWN", 0.0);
                    }
                    break;
                }
            }
            pthread_mutex_unlock(&G_ST->state_mutex);
            usleep(50*1000); // small gap between probes
        }
        usleep(2000*1000); // 2s between sweeps
    }
    return NULL;
}

static void handle_ss(int fd, const char *peer_ip) {
    // First line: REGISTER_SS <port>
    char line[PROTO_MAX_LINE];
    ssize_t n = su_recv_line(fd, line, sizeof(line));
    if (n <= 0) { su_close(fd); return; }
    char *tok[4]; int tc; char tmp[PROTO_MAX_LINE]; snprintf(tmp, sizeof(tmp), "%s", line); tc = str_split_ws(tmp, tok, 4);
    if (tc == 2 && strcmp(tok[0], "REGISTER_SS") == 0) {
        uint16_t port = (uint16_t)atoi(tok[1]);
        nm_add_ss(G_ST, peer_ip, port);
        proto_send_ok(fd);
        
        // Receive file synchronization list from SS
        char sync_line[PROTO_MAX_LINE];
        ssize_t m = su_recv_line(fd, sync_line, sizeof(sync_line));
        if (m > 0 && str_startswith(sync_line, "SYNC_FILES ")) {
            int file_count = atoi(sync_line + 11);
            nm_info("SS %s:%u syncing %d files", peer_ip, port, file_count);
            
            pthread_mutex_lock(&G_ST->state_mutex);
            for (int i = 0; i < file_count; i++) {
                char filename[PROTO_MAX_LINE];
                ssize_t fn = su_recv_line(fd, filename, sizeof(filename));
                if (fn <= 0) break;
                str_trim(filename);
                
                // Check if NM already knows about this file
                FileMeta *existing = nm_get_file(G_ST, filename);
                if (!existing) {
                    // Create metadata entry for discovered file
                    FileMeta *fm = (FileMeta*)calloc(1, sizeof(FileMeta));
                    fm->name = strdup(filename);
                    fm->owner = strdup("unknown");  // Will be updated on first access
                    fm->created = fm->modified = fm->last_access = time(NULL);
                    
                    // Add this SS as storage location
                    StorageServerRef *ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                    ssr->host = strdup(peer_ip);
                    ssr->port = port;
                    fm->storage_servers = ssr;
                    
                    // Grant owner RW access (will be corrected on first use)
                    nm_acl_grant(fm, "unknown", true);
                    
                    hm_put(G_ST->files, fm->name, fm, NULL);
                    nm_info("Discovered file from SS: %s", filename);
                } else {
                    // File exists - check if this SS is in the list
                    bool found = false;
                    StorageServerRef *ssr = existing->storage_servers;
                    while (ssr) {
                        if (strcmp(ssr->host, peer_ip) == 0 && ssr->port == port) {
                            found = true;
                            break;
                        }
                        ssr = ssr->next;
                    }
                    
                    // Add this SS if not already listed
                    if (!found) {
                        StorageServerRef *new_ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                        new_ssr->host = strdup(peer_ip);
                        new_ssr->port = port;
                        new_ssr->next = existing->storage_servers;
                        existing->storage_servers = new_ssr;
                        nm_info("Added SS to existing file: %s", filename);
                    }
                }
            }
            nm_state_save(G_ST);  // Persist discovered files
            pthread_mutex_unlock(&G_ST->state_mutex);
            
            proto_send_ok(fd);  // Acknowledge sync completion
        }
    }
    su_close(fd);
}

static void *conn_thread(void *arg) {
    ConnInfo *ci = (ConnInfo*)arg;
    int fd = ci->fd;
    char first[PROTO_MAX_LINE];
    ssize_t n = su_recv_line(fd, first, sizeof(first));
    if (n <= 0) { su_close(fd); free(ci); return NULL; }
    if (str_startswith(first, "NOTIFY_WRITE ")) {
        // Format: NOTIFY_WRITE <file> <user> <words> <chars>
        char fname[256], user[256]; unsigned long words=0, chars=0;
        if (sscanf(first, "NOTIFY_WRITE %255s %255s %lu %lu", fname, user, &words, &chars) == 4) {
            pthread_mutex_lock(&G_ST->state_mutex);
            FileMeta *fm = nm_get_file(G_ST, fname);
            if (fm) {
                fm->words = (size_t)words;
                fm->chars = (size_t)chars;
                fm->modified = time(NULL);
                free(fm->last_accessor); fm->last_accessor = strdup(user);
                fm->last_access = fm->modified;
                nm_state_save(G_ST);
            }
            pthread_mutex_unlock(&G_ST->state_mutex);
            proto_send_ok(fd); su_send_line(fd, ".");
        }
        su_close(fd); free(ci); return NULL;
    }
    if (str_startswith(first, "REGISTER_SS ")) {
        // Rewind by faking this line back to stream: we already parsed, call handler on this fd after pushing back? Simpler: handle_ss expects to read again, so pass using a temp socket: but can't. Instead, handle_ss logic integrated here.
        char *tok[4]; int tc; char tmp[PROTO_MAX_LINE]; snprintf(tmp, sizeof(tmp), "%s", first); tc = str_split_ws(tmp, tok, 4);
        if (tc == 2) {
            uint16_t port = (uint16_t)atoi(tok[1]);
            nm_add_ss(G_ST, ci->peer_ip, port);
            nm_info("REGISTER_SS from %s:%u -> port=%u", ci->peer_ip, ci->peer_port, port);
            proto_send_ok(fd);
            
            // Receive file synchronization list from SS
            char sync_line[PROTO_MAX_LINE];
            ssize_t m = su_recv_line(fd, sync_line, sizeof(sync_line));
            if (m > 0 && str_startswith(sync_line, "SYNC_FILES ")) {
                int file_count = atoi(sync_line + 11);
                nm_info("SS %s:%u syncing %d files", ci->peer_ip, port, file_count);
                
                pthread_mutex_lock(&G_ST->state_mutex);
                for (int i = 0; i < file_count; i++) {
                    char filename[PROTO_MAX_LINE];
                    ssize_t fn = su_recv_line(fd, filename, sizeof(filename));
                    if (fn <= 0) break;
                    str_trim(filename);
                    
                    // Check if NM already knows about this file
                    FileMeta *existing = nm_get_file(G_ST, filename);
                    if (!existing) {
                        // Create metadata entry for discovered file
                        FileMeta *fm = (FileMeta*)calloc(1, sizeof(FileMeta));
                        fm->name = strdup(filename);
                        fm->owner = strdup("unknown");  // Will be updated on first access
                        fm->created = fm->modified = fm->last_access = time(NULL);
                        
                        // Add this SS as storage location
                        StorageServerRef *ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                        ssr->host = strdup(ci->peer_ip);
                        ssr->port = port;
                        fm->storage_servers = ssr;
                        
                        // Grant owner RW access (will be corrected on first use)
                        nm_acl_grant(fm, "unknown", true);
                        
                        hm_put(G_ST->files, fm->name, fm, NULL);
                        nm_info("Discovered file from SS: %s", filename);
                    } else {
                        // File exists - check if this SS is in the list
                        bool found = false;
                        StorageServerRef *ssr = existing->storage_servers;
                        while (ssr) {
                            if (strcmp(ssr->host, ci->peer_ip) == 0 && ssr->port == port) {
                                found = true;
                                break;
                            }
                            ssr = ssr->next;
                        }
                        
                        // Add this SS if not already listed
                        if (!found) {
                            StorageServerRef *new_ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                            new_ssr->host = strdup(ci->peer_ip);
                            new_ssr->port = port;
                            new_ssr->next = existing->storage_servers;
                            existing->storage_servers = new_ssr;
                            nm_info("Added SS to existing file: %s", filename);
                        }
                    }
                }
                nm_state_save(G_ST);  // Persist discovered files
                pthread_mutex_unlock(&G_ST->state_mutex);
                
                proto_send_ok(fd);  // Acknowledge sync completion
            }
        }
        su_close(fd);
    } else {
        // Treat as client and process the first line followed by the rest, mirroring handle_client
        char username[128] = "";
        char current[PROTO_MAX_LINE]; snprintf(current, sizeof(current), "%s", first);
        char line[PROTO_MAX_LINE];
        int have_current = 1;
        while (1) {
            if (!have_current) {
                ssize_t m = su_recv_line(fd, current, sizeof(current));
                if (m == -2 || m <= 0) break;
            }
            have_current = 0;
            str_trim(current);
            if (!*current) continue;
            nm_info("REQ user=%s from %s:%u -> %s", *username?username:"-", ci->peer_ip, ci->peer_port, current);
            char *tokens[8]; char tmp2[PROTO_MAX_LINE];
            snprintf(tmp2, sizeof(tmp2), "%s", current);
            int tc2 = str_split_ws(tmp2, tokens, 8);
            if (tc2 <= 0) { proto_send_err(fd, ERR_BAD_REQUEST, "Empty"); continue; }

            if (strcmp(tokens[0], "REGISTER") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing username"); continue; }
                snprintf(username, sizeof(username), "%s", tokens[1]);
                nm_register_user(G_ST, username);
                nm_info("Client registered: %s", username);
                proto_send_ok(fd);
                continue;
            }
            if (!*username) { proto_send_err(fd, ERR_UNAUTHORIZED, "Register first"); continue; }

            // Duplicate of core client handler, simplified to avoid extra refactor
            if (strcmp(tokens[0], "LIST") == 0) {
                proto_send_ok(fd);
                for (size_t i = 0; i < G_ST->users->nbuckets; ++i) {
                    HMEntry *e = G_ST->users->buckets[i];
                    while (e) { su_send_line(fd, e->key); e = e->next; }
                }
                su_send_line(fd, ".");
                continue;
            } else if (strcmp(tokens[0], "USERS") == 0) {
                proto_send_ok(fd);
                pthread_mutex_lock(&G_ST->state_mutex);
                su_send_line(fd, "=== ACTIVE USERS ===");
                UserSession *active = nm_get_active_users(G_ST);
                if (!active) {
                    su_send_line(fd, "No active users");
                } else {
                    while (active) {
                        char time_str[64];
                        struct tm *tm_info = localtime(&active->login_time);
                        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
                        char user_info[256];
                        snprintf(user_info, sizeof(user_info), "[ACTIVE] %s (logged in: %s)", active->username, time_str);
                        su_send_line(fd, user_info);
                        active = active->next;
                    }
                }
                su_send_line(fd, "");
                su_send_line(fd, "=== ALL REGISTERED USERS ===");
                UserSession *all_users = nm_get_all_users(G_ST);
                if (!all_users) {
                    su_send_line(fd, "No users registered");
                } else {
                    while (all_users) {
                        char last_seen[64];
                        struct tm *tm_info = localtime(&all_users->last_activity);
                        strftime(last_seen, sizeof(last_seen), "%Y-%m-%d %H:%M:%S", tm_info);
                        char user_info[256];
                        snprintf(user_info, sizeof(user_info), "[%s] %s (last seen: %s)", all_users->is_active ? "ACTIVE" : "OFFLINE", all_users->username, last_seen);
                        su_send_line(fd, user_info);
                        all_users = all_users->next;
                    }
                }
                pthread_mutex_unlock(&G_ST->state_mutex);
                su_send_line(fd, ".");
                continue;
            } else if (strcmp(tokens[0], "VIEW") == 0) {
                int all = 0, detailed = 0;
                for (int i = 1; i < tc2; ++i) {
                    if (strcmp(tokens[i], "-a") == 0) { all = 1; continue; }
                    if (strcmp(tokens[i], "-l") == 0) { detailed = 1; continue; }
                    if (tokens[i][0] == '-') {
                        for (const char *p = tokens[i] + 1; *p; ++p) {
                            if (*p == 'a') all = 1;
                            else if (*p == 'l') detailed = 1;
                        }
                    }
                }
                proto_send_ok(fd);
                if (detailed) {
                    su_send_line(fd, "---------------------------------------------------------");
                    su_send_line(fd, "| Filename   | Words | Chars | Last Access        | Owner |");
                    su_send_line(fd, "---------------------------------------------------------");
                }
                for (size_t i = 0; i < G_ST->files->nbuckets; ++i) {
                    HMEntry *e = G_ST->files->buckets[i];
                    while (e) {
                        FileMeta *fm = (FileMeta*)e->value;
                        if (all || nm_user_has_read(fm, username)) {
                            if (detailed) {
                                char b[512], ts[64];
                                format_time(fm->last_access, ts, sizeof(ts));
                                snprintf(b, sizeof(b), "| %-10s | %5zu | %5zu | %-18s | %-5s |", fm->name, fm->words, fm->chars, ts, fm->owner);
                                su_send_line(fd, b);
                            } else su_send_line(fd, fm->name);
                        }
                        e = e->next;
                    }
                }
                if (detailed) su_send_line(fd, "---------------------------------------------------------");
                su_send_line(fd, ".");
                continue;
            } else if (strcmp(tokens[0], "CREATE") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                if (strlen(tokens[1]) > 255) { proto_send_err(fd, ERR_BAD_REQUEST, "Filename too long"); continue; }
                pthread_mutex_lock(&G_ST->state_mutex);
                if (nm_get_file(G_ST, tokens[1])) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "File exists"); continue; }
                StorageServer *ss = nm_pick_ss(G_ST);
                if (!ss) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "CREATE %s %s", tokens[1], username);
                
                // Create file metadata with multiple storage servers
                FileMeta *fm = nm_create_file(G_ST, tokens[1], username);
                if (!fm || !fm->storage_servers) {
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_err(fd, ERR_INTERNAL, "No storage servers available");
                    continue;
                }
                
                // Send CREATE request to all selected storage servers
                int success_count = 0; int total_replicas = 0;
                StorageServerRef *ssr = fm->storage_servers;
                while (ssr) {
                    total_replicas++;
                    int rc = ss_send_simple(ssr->host, ssr->port, req, NULL);
                    if (rc == 0) success_count++;
                    ssr = ssr->next;
                }

                int required = (total_replicas >= 2) ? 2 : total_replicas;
                if (required > 0 && success_count >= required) {
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_text_block(fd, "File Created Successfully!");
                } else {
                    nm_delete_file(G_ST, tokens[1]);
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_err(fd, ERR_INTERNAL, "Failed to create file on required replicas");
                }
            } else if (strcmp(tokens[0], "READ") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fm->name);
                char *text = NULL; int rc = ss_send_to_all(fm, req, &text);
                if (rc == 0) {
                    pthread_mutex_lock(&G_ST->state_mutex);
                    update_stats_from_text(fm, text, username);
                    nm_state_save(G_ST);
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_text_block(fd, text);
                    free(text);
                } else proto_send_err(fd, ERR_INTERNAL, "SS error");
                continue;
            } else if (strcmp(tokens[0], "DELETE") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "DELETE %s", fm->name);
                char *errtxt = NULL; int rc = ss_send_to_all(fm, req, &errtxt);
                if (rc == 0) {
                    pthread_mutex_lock(&G_ST->state_mutex);
                    nm_delete_file(G_ST, fm->name);
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_text_block(fd, "File deleted successfully!");
                } else {
                    if (errtxt && strncmp(errtxt, "ERR", 3) == 0) {
                        const char *msg = strstr(errtxt, " ");
                        msg = msg ? strstr(msg+1, " ") : NULL;
                        if (msg && *msg) {
                            msg++;
                            if (strstr(msg, "Locked")) proto_send_err(fd, ERR_LOCKED, msg);
                            else if (strstr(msg, "No file")) proto_send_err(fd, ERR_NOT_FOUND, msg);
                            else proto_send_err(fd, ERR_INTERNAL, msg);
                        } else proto_send_err(fd, ERR_INTERNAL, "SS error");
                    } else proto_send_err(fd, ERR_INTERNAL, "SS error");
                }
                free(errtxt);
                continue;
            } else if (strcmp(tokens[0], "INFO") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                proto_send_ok(fd);
                char b[512], ts[64];
                snprintf(b, sizeof(b), "File: %s", fm->name); su_send_line(fd, b);
                snprintf(b, sizeof(b), "Owner: %s", fm->owner); su_send_line(fd, b);
                format_time(fm->created, ts, sizeof(ts)); snprintf(b, sizeof(b), "Created: %s", ts); su_send_line(fd, b);
                format_time(fm->modified, ts, sizeof(ts)); snprintf(b, sizeof(b), "Last Modified: %s", ts); su_send_line(fd, b);
                // Match comprehensive_tests.sh expectation: exact 'Size: N'
                snprintf(b, sizeof(b), "Size: %zu", fm->chars); su_send_line(fd, b);
                if (fm->last_accessor) { format_time(fm->last_access, ts, sizeof(ts)); snprintf(b, sizeof(b), "Last Accessed: %s by %s", ts, fm->last_accessor); su_send_line(fd, b); }
                su_send_line(fd, "Access:");
                for (Access *a = fm->acl; a; a = a->next) { snprintf(b, sizeof(b), "  %s (%s)", a->user, a->can_write?"RW":"R"); su_send_line(fd, b); }
                su_send_line(fd, ".");
                continue;
            } else if (strcmp(tokens[0], "ADDACCESS") == 0) {
                if (tc2 < 4) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: ADDACCESS -R|-W <file> <user>"); continue; }
                bool write = strcmp(tokens[1], "-W") == 0;
                if (!write && strcmp(tokens[1], "-R") != 0) { proto_send_err(fd, ERR_BAD_REQUEST, "Flag"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[2]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
                // Validate that target user exists
                if (!nm_user_exists(G_ST, tokens[3])) {
                    proto_send_err(fd, ERR_NOT_FOUND, "Invalid user or user does not exist");
                    continue;
                }
                pthread_mutex_lock(&G_ST->state_mutex);
                bool has_read = nm_user_has_read(fm, tokens[3]);
                bool has_write = nm_user_has_write(fm, tokens[3]);
                if (write) {
                    if (has_write) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "User already has this access"); continue; }
                    nm_acl_grant(fm, tokens[3], true);
                    nm_state_save(G_ST);
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_text_block(fd, has_read ? "Access upgraded to write" : "Access granted successfully!");
                } else {
                    if (has_read) { pthread_mutex_unlock(&G_ST->state_mutex); proto_send_err(fd, ERR_CONFLICT, "User already has this access"); continue; }
                    nm_acl_grant(fm, tokens[3], false);
                    nm_state_save(G_ST);
                    pthread_mutex_unlock(&G_ST->state_mutex);
                    proto_send_text_block(fd, "Access granted successfully!");
                }
                continue;
            } else if (strcmp(tokens[0], "REMACCESS") == 0) {
                if (tc2 < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: REMACCESS <file> <user>"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (strcmp(fm->owner, username) != 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner only"); continue; }
                if (strcmp(fm->owner, tokens[2]) == 0) { proto_send_err(fd, ERR_FORBIDDEN, "Owner cannot remove their own access."); continue; }
                pthread_mutex_lock(&G_ST->state_mutex);
                if (!nm_user_has_read(fm, tokens[2])) { pthread_mutex_unlock(&G_ST->state_mutex); char emsg[256]; snprintf(emsg, sizeof(emsg), "User %s never had this access", tokens[2]); proto_send_err(fd, ERR_NOT_FOUND, emsg); continue; }
                nm_acl_revoke(fm, tokens[2]);
                nm_state_save(G_ST);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, "Access removed successfully!");
                continue;
            } else if (strcmp(tokens[0], "WRITE") == 0) {
                if (tc2 < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: WRITE <file> <sentence_index>"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (!nm_user_has_write(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No write access"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "WRITE %s %s %s", fm->name, tokens[2], username);
                
                StorageServerRef *server = get_available_server(fm);
                if (!server) { proto_send_err(fd, ERR_INTERNAL, "No available servers"); continue; }
                
                int fdss = su_connect(server->host, server->port);
                if (fdss < 0) { proto_send_err(fd, ERR_INTERNAL, "SS connect"); continue; }
                if (su_send_line(fdss, req) < 0) { su_close(fdss); proto_send_err(fd, ERR_INTERNAL, "SS send"); continue; }
                char resp0[PROTO_MAX_LINE];
                ssize_t r00 = su_recv_line(fdss, resp0, sizeof(resp0));
                if (r00 <= 0) { su_close(fdss); proto_send_err(fd, ERR_INTERNAL, "SS no response"); continue; }
                if (strncmp(resp0, "ERR", 3) == 0) { proto_send_err(fd, ERR_CONFLICT, resp0 + 4); su_close(fdss); continue; }
                proto_send_ok(fd);
                while (1) {
                    ssize_t m = su_recv_line(fd, line, sizeof(line));
                    if (m <= 0) { su_close(fdss); break; }
                    if (su_send_line(fdss, line) < 0) { su_close(fdss); break; }
                    if (strcmp(line, "ETIRW") == 0) {
                        char resp2[PROTO_MAX_LINE];
                        ssize_t r = su_recv_line(fdss, resp2, sizeof(resp2));
                        su_close(fdss);
                        if (r > 0 && strncmp(resp2, "OK", 2) == 0) {
                            // CRITICAL: Synchronize content to ALL replicas after write
                            // Read content from the primary server we just wrote to
                            int sync_fd = su_connect(server->host, server->port);
                            if (sync_fd >= 0) {
                                char read_req[PROTO_MAX_LINE]; snprintf(read_req, sizeof(read_req), "READ %s", fm->name);
                                su_send_line(sync_fd, read_req);
                                char sync_resp[PROTO_MAX_LINE];
                                if (su_recv_line(sync_fd, sync_resp, sizeof(sync_resp)) > 0 && strncmp(sync_resp, "OK", 2) == 0) {
                                    // Connect to all OTHER replicas and sync content
                                    typedef struct { int fd; const char *host; uint16_t port; } SyncDest;
                                    SyncDest sync_dests[32]; int sync_count = 0;
                                    for (StorageServerRef *ssr = fm->storage_servers; ssr && sync_count < 32; ssr = ssr->next) {
                                        if (ssr == server) continue; // Skip the primary we just wrote to
                                        int dest_fd = su_connect(ssr->host, ssr->port);
                                        if (dest_fd < 0) continue;
                                        char sync_cmd[PROTO_MAX_LINE]; snprintf(sync_cmd, sizeof(sync_cmd), "SYNC_FILE %s", fm->name);
                                        if (su_send_line(dest_fd, sync_cmd) < 0) { su_close(dest_fd); continue; }
                                        sync_dests[sync_count].fd = dest_fd; sync_dests[sync_count].host = ssr->host; sync_dests[sync_count].port = ssr->port; sync_count++;
                                    }
                                    // Stream content from primary to all replicas
                                    size_t w_count = 0, c_count = 0; int in_w = 0;
                                    char content_line[PROTO_MAX_LINE];
                                    while (1) {
                                        ssize_t ln = su_recv_line(sync_fd, content_line, sizeof(content_line));
                                        if (ln <= 0) break;
                                        if (strcmp(content_line, ".") == 0) {
                                            for (int i = 0; i < sync_count; ++i) {
                                                su_send_line(sync_dests[i].fd, ".");
                                                char ack[PROTO_MAX_LINE];
                                                if (su_recv_line(sync_dests[i].fd, ack, sizeof(ack)) > 0) {
                                                    su_recv_line(sync_dests[i].fd, ack, sizeof(ack)); // drain trailing '.'
                                                }
                                                su_close(sync_dests[i].fd);
                                            }
                                            break;
                                        }
                                        // Forward to all replicas
                                        for (int i = 0; i < sync_count; ++i) su_send_line(sync_dests[i].fd, content_line);
                                        // Update stats
                                        for (char *p = content_line; *p; ++p) {
                                            if (*p > ' ' && *p <= '~') { if (!in_w) { in_w = 1; w_count++; } }
                                            else { in_w = 0; }
                                            c_count++;
                                        }
                                        c_count++; in_w = 0; // newline
                                    }
                                    su_close(sync_fd);
                                    // Update metadata
                                    pthread_mutex_lock(&G_ST->state_mutex);
                                    fm->chars = c_count;
                                    fm->words = w_count;
                                    fm->last_access = time(NULL);
                                    free(fm->last_accessor); fm->last_accessor = strdup(username);
                                    update_last_modified(fm);
                                    if (strcmp(fm->owner, "unknown") == 0) {
                                        free(fm->owner); fm->owner = strdup(username);
                                    }
                                    nm_state_save(G_ST);
                                    pthread_mutex_unlock(&G_ST->state_mutex);
                                } else {
                                    su_close(sync_fd);
                                }
                            }
                            proto_send_text_block(fd, "Write successful");
                        }
                        else if (r > 0 && strncmp(resp2, "ERR", 3) == 0) proto_send_err(fd, ERR_RANGE, resp2 + 4);
                        else proto_send_err(fd, ERR_INTERNAL, "Write failed");
                        break;
                    }
                }
                continue;
            } else if (strcmp(tokens[0], "REFRESH") == 0) {
                // REFRESH <file> <mode> <user>
                if (tc2 < 4) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: REFRESH <file> <mode> <user>"); continue; }
                const char *fname = tokens[1]; const char *mode = tokens[2]; const char *actor = tokens[3];
                FileMeta *fm = nm_get_file(G_ST, fname);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                StorageServerRef *source = get_available_server(fm);
                if (!source) { proto_send_err(fd, ERR_INTERNAL, "No storage server"); continue; }
                int sfd = su_connect(source->host, source->port);
                if (sfd < 0) { proto_send_err(fd, ERR_INTERNAL, "Source connect failed"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fm->name);
                if (su_send_line(sfd, req) < 0) { su_close(sfd); proto_send_err(fd, ERR_INTERNAL, "Source send failed"); continue; }
                char r0[PROTO_MAX_LINE];
                if (su_recv_line(sfd, r0, sizeof(r0)) <= 0 || strncmp(r0, "OK", 2) != 0) {
                    su_close(sfd);
                    proto_send_err(fd, ERR_INTERNAL, "Source read failed");
                    continue;
                }
                typedef struct { int fd; const char *host; uint16_t port; } Dest;
                Dest dests[32]; int nd = 0;
                for (StorageServerRef *ssr = fm->storage_servers; ssr && nd < 32; ssr = ssr->next) {
                    if (ssr == source) continue;
                    int cfd = su_connect(ssr->host, ssr->port);
                    if (cfd < 0) continue;
                    char sync_cmd[PROTO_MAX_LINE]; snprintf(sync_cmd, sizeof(sync_cmd), "SYNC_FILE %s", fm->name);
                    if (su_send_line(cfd, sync_cmd) < 0) { su_close(cfd); continue; }
                    dests[nd].fd = cfd; dests[nd].host = ssr->host; dests[nd].port = ssr->port; nd++;
                }
                size_t total_chars = 0, total_words = 0; int in_word = 0;
                char linebuf[PROTO_MAX_LINE];
                while (1) {
                    ssize_t rn = su_recv_line(sfd, linebuf, sizeof(linebuf));
                    if (rn <= 0) { break; }
                    if (strcmp(linebuf, ".") == 0) {
                        for (int i = 0; i < nd; ++i) {
                            su_send_line(dests[i].fd, ".");
                            char okb[PROTO_MAX_LINE];
                            if (su_recv_line(dests[i].fd, okb, sizeof(okb)) > 0) {
                                su_recv_line(dests[i].fd, okb, sizeof(okb));
                            }
                            su_close(dests[i].fd);
                        }
                        break;
                    }
                    for (int i = 0; i < nd; ++i) su_send_line(dests[i].fd, linebuf);
                    for (char *p = linebuf; *p; ++p) {
                        if (*p > ' ' && *p <= '~') { if (!in_word) { in_word = 1; total_words++; } }
                        else { in_word = 0; }
                        total_chars++;
                    }
                    total_chars++; in_word = 0;
                }
                su_close(sfd);
                pthread_mutex_lock(&G_ST->state_mutex);
                fm->chars = total_chars;
                fm->words = total_words;
                fm->last_access = time(NULL);
                free(fm->last_accessor); fm->last_accessor = strdup(actor);
                if ((strcmp(mode, "WRITE") == 0) || (strcmp(mode, "UNDO") == 0)) update_last_modified(fm);
                if (strcmp(fm->owner, "unknown") == 0 && strcmp(mode, "WRITE") == 0) {
                    free(fm->owner); fm->owner = strdup(actor);
                }
                nm_state_save(G_ST);
                pthread_mutex_unlock(&G_ST->state_mutex);
                proto_send_text_block(fd, "Refresh OK");
                continue;
            } else if (strcmp(tokens[0], "UNDO") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (!nm_user_has_write(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No write access"); continue; }
                StorageServerRef *server = get_available_server(fm);
                if (!server) { proto_send_err(fd, ERR_INTERNAL, "No available servers"); continue; }
                proto_send_ok(fd);
                char out[256]; snprintf(out, sizeof(out), "CONNECT %s %u %s", server->host, server->port, fm->name);
                su_send_line(fd, out);
                su_send_line(fd, ".");
                continue;
            } else if (strcmp(tokens[0], "STREAM") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
                
                StorageServerRef *server = get_available_server(fm);
                if (!server) { proto_send_err(fd, ERR_INTERNAL, "No available servers"); continue; }
                
                int fdss = su_connect(server->host, server->port);
                if (fdss < 0) { proto_send_err(fd, ERR_INTERNAL, "SS connect"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "STREAM %s", fm->name);
                su_send_line(fdss, req);
                proto_send_ok(fd);
                while (1) {
                    ssize_t m = su_recv_line(fdss, line, sizeof(line));
                    if (m <= 0) { proto_send_err(fd, ERR_INTERNAL, "Storage server unavailable during streaming."); break; }
                    su_send_line(fd, line);
                    if (strcmp(line, ".") == 0) break;
                }
                su_close(fdss);
                pthread_mutex_lock(&G_ST->state_mutex);
                fm->last_access = time(NULL);
                free(fm->last_accessor); fm->last_accessor = strdup(username);
                nm_state_save(G_ST);
                pthread_mutex_unlock(&G_ST->state_mutex);
                continue;
            } else if (strcmp(tokens[0], "EXEC") == 0) {
                if (tc2 < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                FileMeta *fm = nm_get_file(G_ST, tokens[1]);
                if (!fm) { proto_send_err(fd, ERR_NOT_FOUND, "No such file"); continue; }
                if (!nm_user_has_read(fm, username)) { proto_send_err(fd, ERR_FORBIDDEN, "No access"); continue; }
                char req[PROTO_MAX_LINE]; snprintf(req, sizeof(req), "READ %s", fm->name);
                char *text = NULL; int rc = ss_send_to_all(fm, req, &text);
                if (rc != 0) { proto_send_err(fd, ERR_INTERNAL, "SS error"); continue; }
                if (!text) text = strdup("");
                int exit_code = 0; char *out = exec_capture_bash(text, &exit_code);
                free(text);
                if (!out) { proto_send_err(fd, ERR_INTERNAL, "Exec failed"); continue; }
                proto_send_text_block(fd, out);
                free(out);
                continue;
            } else if (strcmp(tokens[0], "STATUS") == 0) {
                proto_send_ok(fd);
                pthread_mutex_lock(&G_ST->state_mutex);
                for (StorageServer *s = G_ST->ss_list; s; s = s->next) {
                    char line[256];
                    char t_ok[32]="-", t_ck[32]="-";
                    if (s->last_ok) { time_t t=s->last_ok; struct tm tm; localtime_r(&t,&tm); strftime(t_ok,sizeof(t_ok),"%H:%M:%S", &tm); }
                    if (s->last_check) { time_t t=s->last_check; struct tm tm; localtime_r(&t,&tm); strftime(t_ck,sizeof(t_ck),"%H:%M:%S", &tm); }
                    snprintf(line, sizeof(line), "SS %d %s:%u %s fails=%d last_ok=%s last_ck=%s",
                             s->id, s->host, s->port, s->healthy?"UP":"DOWN", s->fail_count, t_ok, t_ck);
                    su_send_line(fd, line);
                }
                pthread_mutex_unlock(&G_ST->state_mutex);
                su_send_line(fd, ".");
                continue;
            } else {
                proto_send_err(fd, ERR_BAD_REQUEST, "Unknown command");
            }
        }
        
        // Handle user logout when connection closes
        if (*username) {
            pthread_mutex_lock(&G_ST->state_mutex);
            nm_user_logout(G_ST, username);
            pthread_mutex_unlock(&G_ST->state_mutex);
            nm_info("Client disconnected: %s", username);
        }
        
        su_close(fd);
    }
    free(ci);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <data_dir>\n", argv[0]);
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    const char *data_dir = argv[2];

    // Prevent process termination on socket write to closed peer
    signal(SIGPIPE, SIG_IGN);

    log_init("nm.log");
    G_ST = nm_state_load(data_dir);

    // Start health checker thread
    pthread_t hth; pthread_create(&hth, NULL, health_thread, NULL); pthread_detach(hth);

    int sfd = su_listen(port, BACKLOG);
    if (sfd < 0) { perror("listen"); return 1; }
                nm_info("NM listening on port %u", port);

    while (1) {
        ConnInfo *ci = (ConnInfo*)calloc(1, sizeof(ConnInfo));
        ci->fd = su_accept(sfd, ci->peer_ip, sizeof(ci->peer_ip), &ci->peer_port);
        if (ci->fd < 0) { free(ci); if (errno==EINTR) continue; perror("accept"); break; }
        pthread_t th; pthread_create(&th, NULL, conn_thread, ci); pthread_detach(th);
    }

    nm_state_save(G_ST);
    nm_state_free(G_ST);
    log_close();
    return 0;
}
