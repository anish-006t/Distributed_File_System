#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "../common/socket_utils.h"
#include "../common/protocol.h"
#include "../common/strutils.h"
#include "../common/errors.h"
#include "../common/log.h"
#include "../common/file_utils.h"
#include "ss_state.h"
#include "sentence.h"
#include "../common/hashmap.h"
#include <ctype.h>

#define BACKLOG 64

static SSState *G_ST = NULL;
static char G_NM_HOST[128];
static uint16_t G_NM_PORT = 0;
static uint16_t G_SS_PORT = 0;
static pthread_mutex_t G_LOCK_MUTEX = PTHREAD_MUTEX_INITIALIZER;

// Helper functions for sentence-level locking
static bool lock_sentence(FileRec *fr, int sentence_idx, const char *user, int owner_fd) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    // Check if already locked by someone else
    for (SentenceLock *lock = fr->locks; lock; lock = lock->next) {
        if (lock->sentence_idx == sentence_idx && strcmp(lock->user, user) != 0) {
            log_info("LOCK ACQUIRE FAILED: sentence %d already locked by '%s', user '%s' denied", 
                     sentence_idx, lock->user, user);
            pthread_mutex_unlock(&G_LOCK_MUTEX);
            return false;  // Already locked by different user
        }
    }
    // Add lock
    SentenceLock *new_lock = (SentenceLock*)malloc(sizeof(SentenceLock));
    new_lock->sentence_idx = sentence_idx;
    snprintf(new_lock->user, sizeof(new_lock->user), "%s", user);
    new_lock->owner_fd = owner_fd;
    new_lock->next = fr->locks;
    fr->locks = new_lock;
    log_info("LOCK ACQUIRED: sentence %d locked by '%s' (fd=%d)", sentence_idx, user, owner_fd);
    pthread_mutex_unlock(&G_LOCK_MUTEX);
    return true;
}

static void unlock_sentence(FileRec *fr, int sentence_idx, const char *user) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    SentenceLock *prev = NULL;
    SentenceLock *curr = fr->locks;
    while (curr) {
        if (curr->sentence_idx == sentence_idx && strcmp(curr->user, user) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                fr->locks = curr->next;
            }
            free(curr);
            pthread_mutex_unlock(&G_LOCK_MUTEX);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&G_LOCK_MUTEX);
}

// Release all locks held by a given connection (fd) across all files.
static void unlock_all_for_fd(int owner_fd) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    for (size_t i = 0; i < G_ST->files->nbuckets; ++i) {
        HMEntry *e = G_ST->files->buckets[i];
        while (e) {
            FileRec *fr = (FileRec*)e->value;
            SentenceLock *prev = NULL;
            SentenceLock *cur = fr->locks;
            while (cur) {
                if (cur->owner_fd == owner_fd) {
                    SentenceLock *dead = cur;
                    if (prev) prev->next = cur->next; else fr->locks = cur->next;
                    cur = (prev) ? prev->next : fr->locks;
                    free(dead);
                    continue;
                }
                prev = cur; cur = cur->next;
            }
            e = e->next;
        }
    }
    pthread_mutex_unlock(&G_LOCK_MUTEX);
}

static bool is_file_locked(FileRec *fr) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    bool locked = (fr->locks != NULL);
    pthread_mutex_unlock(&G_LOCK_MUTEX);
    return locked;
}

// Helper functions for undo flashcard stack
static void push_undo_flashcard(FileRec *fr, const char *content, const char *user) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    UndoFlashcard *card = (UndoFlashcard*)malloc(sizeof(UndoFlashcard));
    card->content = strdup(content);
    snprintf(card->user, sizeof(card->user), "%s", user);
    card->timestamp = time(NULL);
    card->next = fr->undo_stack;  // Push to top of stack
    fr->undo_stack = card;
    pthread_mutex_unlock(&G_LOCK_MUTEX);
}

static char* pop_undo_flashcard(FileRec *fr, char *user_out, size_t user_out_size) {
    pthread_mutex_lock(&G_LOCK_MUTEX);
    if (!fr->undo_stack) {
        pthread_mutex_unlock(&G_LOCK_MUTEX);
        return NULL;  // No undo available
    }
    UndoFlashcard *card = fr->undo_stack;
    fr->undo_stack = card->next;  // Pop from top
    char *content = card->content;
    if (user_out) {
        snprintf(user_out, user_out_size, "%s", card->user);
    }
    free(card);  // Don't free content, it's returned to caller
    pthread_mutex_unlock(&G_LOCK_MUTEX);
    return content;
}


static int register_with_nm_retry(const char *nm_host, uint16_t nm_port, uint16_t ss_port) {
    // Try to register up to 30 times (~30s), 1s between attempts
    for (int i = 0; i < 30; ++i) {
        int nfd = su_connect(nm_host, nm_port);
        if (nfd >= 0) {
            char reg[256]; snprintf(reg, sizeof(reg), "REGISTER_SS %u", ss_port);
            if (su_send_line(nfd, reg) >= 0) {
                char resp[PROTO_MAX_LINE];
                ssize_t n = su_recv_line(nfd, resp, sizeof(resp));
                if (n > 0 && strncmp(resp, "OK", 2) == 0) {
                    // Send file list to NM for synchronization
                    int file_count = 0;
                    char **files = ss_list_all_files(G_ST, &file_count);
                    
                    if (file_count > 0 && files) {
                        // Send SYNC_FILES command with file list
                        char sync_cmd[256];
                        snprintf(sync_cmd, sizeof(sync_cmd), "SYNC_FILES %d", file_count);
                        su_send_line(nfd, sync_cmd);
                        
                        // Send each filename
                        for (int j = 0; j < file_count; j++) {
                            su_send_line(nfd, files[j]);
                            free(files[j]);
                        }
                        free(files);
                        
                        // Wait for sync acknowledgment
                        su_recv_line(nfd, resp, sizeof(resp));
                    }
                    
                    su_close(nfd);
                    log_info("Registered to NM %s:%u for port %u with %d files", nm_host, nm_port, ss_port, file_count);
                    return 0;
                }
            }
            su_close(nfd);
        }
        log_info("Register to NM failed, retrying...");
        sleep(1);
    }
    log_error("Failed to register to NM after retries");
    return -1;
}

typedef struct { int fd; char ip[64]; uint16_t port; } SSConn;

static void *client_thread(void *arg) {
    SSConn *ci = (SSConn*)arg;
    int fd = ci->fd;
    char line[PROTO_MAX_LINE];
    while (1) {
        ssize_t n = su_recv_line(fd, line, sizeof(line));
        if (n == -2 || n <= 0) break;
        str_trim(line);
        if (!*line) continue;
        log_info("REQ from %s:%u -> %s", ci->ip, ci->port, line);
        char tmp[PROTO_MAX_LINE]; snprintf(tmp, sizeof(tmp), "%s", line);
        char *tok[8]; int tc = str_split_ws(tmp, tok, 8);
        if (tc <= 0) { proto_send_err(fd, ERR_BAD_REQUEST, "Empty"); continue; }

        if (strcmp(tok[0], "CREATE") == 0) {
            if (tc < 3) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: CREATE <file> <owner>"); continue; }
            FileRec *fr = ss_get_or_create_file(G_ST, tok[1]);
            // backup current only if non-empty
            fu_write_all(fr->path, "", 0);
            proto_send_ok(fd);
            su_send_line(fd, ".");
        } else if (strcmp(tok[0], "READ") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileRec *fr = ss_get_or_create_file(G_ST, tok[1]);
            size_t len; char *data = fu_read_all(fr->path, &len);
            if (!data) { proto_send_err(fd, ERR_INTERNAL, "Read fail"); continue; }
            log_info("READ %s -> %zu bytes", tok[1], data ? strlen(data) : 0UL);
            proto_send_text_block(fd, data);
            free(data);
        } else if (strcmp(tok[0], "DELETE") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileRec *fr = ss_get_file(G_ST, tok[1]);
            if (!fr) {
                // If not loaded in-memory, check on-disk existence (e.g., after SS restart)
                char path[1024]; snprintf(path, sizeof(path), "%s/files/%s", G_ST->root, tok[1]);
                if (fu_exists(path)) {
                    fr = ss_get_or_create_file(G_ST, tok[1]);
                } else {
                    proto_send_err(fd, ERR_NOT_FOUND, "No file");
                    continue;
                }
            }
            if (is_file_locked(fr)) { proto_send_err(fd, ERR_LOCKED, "Locked"); continue; }
            if (ss_delete_file(G_ST, tok[1]) == 0) {
                log_info("DELETE %s -> OK", tok[1]);
            }
            proto_send_ok(fd); su_send_line(fd, ".");
    } else if (strcmp(tok[0], "WRITE") == 0) {
            if (tc < 4) { proto_send_err(fd, ERR_BAD_REQUEST, "Usage: WRITE <file> <sentence_index> <user>"); continue; }
            const char *fname = tok[1]; int sidx = atoi(tok[2]); const char *user = tok[3];
            FileRec *fr = ss_get_or_create_file(G_ST, fname);
            
            // Load current content and sentence count to validate sidx
            size_t len0; char *content = fu_read_all(fr->path, &len0); if (!content) content = strdup("");
            // Dynamic sentence split (remove fixed 2048 limit)
            // First pass: count sentences
            int provisional = split_sentences(content, NULL, NULL, 0);
            if (provisional < 0) provisional = 0;
            const char **beg0 = NULL; size_t *sl0 = NULL;
            if (provisional > 0) {
                beg0 = (const char**)malloc(sizeof(char*) * provisional);
                sl0 = (size_t*)malloc(sizeof(size_t) * provisional);
                int sc_tmp = split_sentences(content, beg0, sl0, provisional);
                if (sc_tmp >= 0) provisional = sc_tmp; else provisional = 0;
            }
            int sc0 = provisional;
            if (sidx < 0 || sidx > sc0) {
                free(content);
                if (beg0) free(beg0); if (sl0) free(sl0);
                proto_send_err(fd, ERR_RANGE, "Sentence index out of range.");
                continue;
            }
            
            // Lock this specific sentence (atomic check-and-lock)
            if (!lock_sentence(fr, sidx, user, fd)) {
                free(content);
                if (beg0) free(beg0); if (sl0) free(sl0);
                proto_send_err(fd, ERR_LOCKED, "Sentence locked by another user");
                continue;
            }
            
            log_info("WRITE lock %s sidx=%d by %s", fname, sidx, user);
            
            // Capture OLD content for undo BEFORE modification (spec: only one undo level)
            // Clear any existing undo stack first
            pthread_mutex_lock(&G_LOCK_MUTEX);
            while (fr->undo_stack) {
                UndoFlashcard *card = fr->undo_stack;
                fr->undo_stack = card->next;
                free(card->content);
                free(card);
            }
            pthread_mutex_unlock(&G_LOCK_MUTEX);
            
            // Push current (OLD) content to undo stack
            push_undo_flashcard(fr, content, user);
            
            // ack lock acceptance
            su_send_line(fd, "OK");
            // Buffer edit lines until ETIRW
            typedef struct { int idx; char *text; } Edit;
            int cap = 8, cnt = 0; Edit *edits = (Edit*)malloc(sizeof(Edit)*cap);
            int last_idx_seen = -2147483647; int increasing = 1;
            char edit[PROTO_MAX_LINE];
            while (1) {
                ssize_t m = su_recv_line(fd, edit, sizeof(edit));
                if (m <= 0) {
                    // client/NM disconnected mid-write; unlock and discard changes
                    unlock_sentence(fr, sidx, user);
                    // cleanup any buffered edits
                    if (edits) {
                        for (int i=0;i<cnt;i++) free(edits[i].text);
                        free(edits);
                    }
                    free(content);
                    // do not send response, connection is gone
                    break;
                }
                if (strcmp(edit, "ETIRW") == 0) {
                    // CRITICAL: Re-read file to get current state (handles concurrent writes)
                    // Other users may have modified other sentences while we held the lock on this sentence
                    free(content); // Free stale content
                    size_t current_len;
                    content = fu_read_all(fr->path, &current_len);
                    if (!content) content = strdup("");
                    
                    // Commit buffered edits in a single apply step
                    int sc_est = split_sentences(content, NULL, NULL, 0);
                    const char **beg = NULL; size_t *sl = NULL; int sc = sc_est;
                    if (sc_est > 0) {
                        beg = (const char**)malloc(sizeof(char*) * sc_est);
                        sl = (size_t*)malloc(sizeof(size_t) * sc_est);
                        int sc_real = split_sentences(content, beg, sl, sc_est);
                        if (sc_real >= 0) sc = sc_real; else sc = 0;
                    }

                    // Extract base sentence from CURRENT file state
                    char *base = NULL;
                    if (sidx >= 0 && sidx < sc && beg && sl) {
                        base = (char*)malloc(sl[sidx]+1);
                        memcpy(base, beg[sidx], sl[sidx]); base[sl[sidx]]='\0';
                    } else {
                        base = strdup("");
                    }

                    // Sort edits by idx ascending
                    if (cnt > 1) {
                        for (int i=1;i<cnt;i++) { if (!(edits[i].idx > edits[i-1].idx)) { increasing = 0; break; } }
                        // simple insertion sort
                        for (int i=1;i<cnt;i++) {
                            Edit e = edits[i]; int j=i-1; while (j>=0 && edits[j].idx > e.idx) { edits[j+1]=edits[j]; j--; } edits[j+1]=e;
                        }
                    }

                    char *new_sent = NULL;
                    if (cnt == 0) {
                        new_sent = strdup(base);
                    } else if (increasing) {
                        // sequence mode: concatenate texts in order and insert once at smallest index
                        size_t total = 0; for (int i=0;i<cnt;i++) total += strlen(edits[i].text) + 1;
                        char *joined = (char*)malloc(total+1); joined[0]='\0';
                        for (int i=0;i<cnt;i++) {
                            if (i>0) strcat(joined, " ");
                            strcat(joined, edits[i].text);
                        }
                        // determine insertion index (clamp later inside helper)
                        int base_idx = edits[0].idx;
                        new_sent = insert_words_into_sentence(base, base_idx, joined);
                        free(joined);
                    } else {
                        // apply one by one on a working buffer
                        char *work = strdup(base);
                        for (int i=0;i<cnt;i++) {
                            char *next = insert_words_into_sentence(work, edits[i].idx, edits[i].text);
                            free(work);
                            work = next;
                        }
                        new_sent = work;
                    }

                    // Rebuild full content with new_sent at position sidx (or append)
                    if (sidx == sc) {
                        size_t newcap = strlen(content) + strlen(new_sent) + 4;
                        char *re = (char*)malloc(newcap); re[0]='\0';
                        if (strlen(content) > 0) {
                            strncat(re, content, newcap - strlen(re) - 1);
                            strncat(re, " ", newcap - strlen(re) - 1);
                        }
                        strncat(re, new_sent, newcap - strlen(re) - 1);
                        free(content); content = re;
                    } else {
                        size_t newcap = strlen(content) + strlen(new_sent) + 64;
                        char *re = (char*)malloc(newcap); re[0]='\0';
                        if (beg && sl) {
                            for (int i=0;i<sc;i++) {
                                if (i==sidx) {
                                    strncat(re, new_sent, newcap - strlen(re) - 1);
                                } else {
                                    size_t rem = newcap - strlen(re) - 1;
                                    size_t copy = sl[i] < rem ? sl[i] : rem;
                                    strncat(re, beg[i], copy);
                                }
                                if (i < sc-1) strncat(re, " ", newcap - strlen(re) - 1);
                            }
                        }
                        free(content); content = re;
                    }

                    // cleanup
                    free(base);
                    if (beg) free(beg); if (sl) free(sl);
                    for (int i=0;i<cnt;i++) free(edits[i].text);
                    free(edits);

                    // CRITICAL: Check if new content contains sentence delimiters
                    // If yes, we need to inform the NM to re-index the file
                    int has_delimiter = (strpbrk(content, ".!?") != NULL);
                    
                    // save and unlock
                    size_t clen = strlen(content);
                    fu_write_all(fr->path, content, clen);
                    unlock_sentence(fr, sidx, user);
                    log_info("WRITE commit %s sidx=%d by %s -> len=%zu sentences=%d%s", 
                             fname, sidx, user, clen, sc,
                             has_delimiter ? " (contains delimiters)" : "");
                    free(content);
                    
                    // Notify NM of metadata (words/chars) to keep metadata fresh even if client fails before REFRESH
                    {
                        // Compute word count quickly on new content (re-read file content just saved)
                        size_t ml; char *meta_text = fu_read_all(fr->path, &ml); if (!meta_text) meta_text = strdup("");
                        size_t words = 0; int inw = 0; for (char *p = meta_text; *p; ++p) { if (*p > ' ' && *p <= '~') { if (!inw) { inw=1; words++; } } else inw=0; }
                        int nfd = su_connect(G_NM_HOST, G_NM_PORT);
                        if (nfd >= 0) {
                            char notify[512]; snprintf(notify, sizeof(notify), "NOTIFY_WRITE %s %s %zu %zu", fname, user, words, ml);
                            su_send_line(nfd, notify);
                            // best-effort: read and ignore response
                            char resp[PROTO_MAX_LINE]; su_recv_line(nfd, resp, sizeof(resp));
                            su_close(nfd);
                        }
                        free(meta_text);
                    }

                    if (has_delimiter) {
                        // Send OK with a note that file now has multiple sentences
                        su_send_line(fd, "OK File now contains sentence delimiters and may have new sentences");
                    } else {
                        su_send_line(fd, "OK");
                    }
                    break;
                }
                // parse: <index> <text> or continuation line when the first token isn't a pure integer
                char *sp = strchr(edit, ' ');
                bool is_pure_int = false;
                if (sp) {
                    is_pure_int = true;
                    for (char *p = edit; p < sp; ++p) {
                        if (!isdigit((unsigned char)*p) && !(p == edit && (*p == '+' || *p == '-'))) {
                            is_pure_int = false;
                            break;
                        }
                    }
                }
                if (!sp || !is_pure_int) {
                    // Treat as continuation of the previous edit (for long inputs split across lines)
                    if (cnt > 0) {
                        size_t oldlen = strlen(edits[cnt-1].text);
                        size_t addlen = strlen(edit);
                        char *grown = (char*)realloc(edits[cnt-1].text, oldlen + 1 + addlen + 1);
                        if (grown) {
                            edits[cnt-1].text = grown;
                            edits[cnt-1].text[oldlen] = ' ';
                            memcpy(edits[cnt-1].text + oldlen + 1, edit, addlen);
                            edits[cnt-1].text[oldlen + 1 + addlen] = '\0';
                        }
                    }
                    continue;
                }
                *sp = '\0'; int widx = atoi(edit); const char *ins = sp + 1;
                
                // Spec requires: word index validation - negative or > word_count+1 should error
                // Get current sentence word count for validation
                int sent_est = split_sentences(content, NULL, NULL, 0);
                const char **sent_beg = NULL; size_t *sent_len = NULL; int sent_count = sent_est;
                if (sent_est > 0) {
                    sent_beg = (const char**)malloc(sizeof(char*) * sent_est);
                    sent_len = (size_t*)malloc(sizeof(size_t) * sent_est);
                    int sc_real = split_sentences(content, sent_beg, sent_len, sent_est);
                    if (sc_real >= 0) sent_count = sc_real; else sent_count = 0;
                }
                
                int max_word_idx = 0;
                if (sidx >= 0 && sidx < sent_count) {
                    // Count words in this sentence
                    char *sent_copy = (char*)malloc(sent_len[sidx] + 1);
                    memcpy(sent_copy, sent_beg[sidx], sent_len[sidx]);
                    sent_copy[sent_len[sidx]] = '\0';
                    
                    char *word = strtok(sent_copy, " \t\r\n");
                    while (word) {
                        max_word_idx++;
                        word = strtok(NULL, " \t\r\n");
                    }
                    free(sent_copy);
                }
                
                // Validate index range: must be >= 0 and <= word_count + 1
                if (widx < 0 || widx > max_word_idx + 1) {
                    // Send error and abort write
                    unlock_sentence(fr, sidx, user);
                    for (int i=0;i<cnt;i++) free(edits[i].text);
                    free(edits);
                    free(content);
                    if (sent_beg) free(sent_beg); if (sent_len) free(sent_len);
                    proto_send_err(fd, ERR_RANGE, "Word index out of range");
                    goto write_done;
                }
                
                // Allow text with delimiters - they will be handled during sentence reconstruction
                // The system will automatically create new sentences when delimiters are present
                
                if (cnt == cap) { cap *= 2; edits = (Edit*)realloc(edits, sizeof(Edit)*cap); }
                edits[cnt].idx = widx; edits[cnt].text = strdup(ins); cnt++;
                if (widx <= last_idx_seen) {
                    increasing = 0;
                }
                last_idx_seen = widx;
                if (sent_beg) free(sent_beg); if (sent_len) free(sent_len);
            }
            write_done:; // Label for error cleanup
        } else if (strcmp(tok[0], "UNDO") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileRec *fr = ss_get_file(G_ST, tok[1]);
            if (!fr) { proto_send_err(fd, ERR_NOT_FOUND, "No file"); continue; }
            if (is_file_locked(fr)) { proto_send_err(fd, ERR_LOCKED, "Write in progress"); continue; }
            
            // Pop the most recent flashcard from the undo stack
            char undo_user[128];
            char *restored_content = pop_undo_flashcard(fr, undo_user, sizeof(undo_user));
            if (!restored_content) {
                proto_send_err(fd, ERR_CONFLICT, "No undo");
                continue;
            }
            
            // Restore the content from the flashcard
            size_t restored_len = strlen(restored_content);
            fu_write_all(fr->path, restored_content, restored_len);
            log_info("UNDO %s - restored to state before write by %s (len=%zu)", 
                     tok[1], undo_user, restored_len);
            free(restored_content);
            
            proto_send_ok(fd); su_send_line(fd, ".");
        } else if (strcmp(tok[0], "STREAM") == 0) {
            if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
            FileRec *fr = ss_get_or_create_file(G_ST, tok[1]);
            size_t len; char *data = fu_read_all(fr->path, &len); if (!data) data = strdup("");
            proto_send_ok(fd);
            // stream word by word with delay
            char *p = data;
            while (*p) {
                while (*p && isspace((unsigned char)*p)) p++;
                if (!*p) break;
                char *start = p;
                while (*p && !isspace((unsigned char)*p)) p++;
                char c = *p; *p = '\0';
                su_send_line(fd, start);
                if (c) { *p = c; }
                usleep(100000); // 0.1s
            }
            su_send_line(fd, ".");
            free(data);
        } else {
            // Handle replication sync from NM
            if (strcmp(tok[0], "SYNC_FILE") == 0) {
                if (tc < 2) { proto_send_err(fd, ERR_BAD_REQUEST, "Missing filename"); continue; }
                const char *fname = tok[1];
                FileRec *fr = ss_get_or_create_file(G_ST, fname);
                // Stream to a temporary file to avoid buffering and ensure atomicity
                char tmppath[1024]; snprintf(tmppath, sizeof(tmppath), "%s.sync.tmp", fr->path);
                FILE *f = fopen(tmppath, "w");
                if (!f) { proto_send_err(fd, ERR_INTERNAL, "Open temp fail"); continue; }
                size_t wlen = 0;
                char sync_line[PROTO_MAX_LINE];
                while (1) {
                    ssize_t sn = su_recv_line(fd, sync_line, sizeof(sync_line));
                    if (sn <= 0) { fclose(f); unlink(tmppath); proto_send_err(fd, ERR_INTERNAL, "Sync read fail"); goto after_sync; }
                    if (strcmp(sync_line, ".") == 0) break;
                    size_t l = strlen(sync_line);
                    if (fwrite(sync_line, 1, l, f) != l) { fclose(f); unlink(tmppath); proto_send_err(fd, ERR_INTERNAL, "Write temp fail"); goto after_sync; }
                    if (fputc('\n', f) == EOF) { fclose(f); unlink(tmppath); proto_send_err(fd, ERR_INTERNAL, "Write temp fail"); goto after_sync; }
                    wlen += l + 1;
                }
                fclose(f);
                // Replace the original file atomically
                if (rename(tmppath, fr->path) != 0) {
                    unlink(tmppath); proto_send_err(fd, ERR_INTERNAL, "Rename fail"); goto after_sync; 
                }
                log_info("SYNC_FILE %s from %s:%u -> %zu bytes", fname, ci->ip, ci->port, wlen);
                proto_send_ok(fd); su_send_line(fd, ".");
            after_sync:
                continue;
            }
            proto_send_err(fd, ERR_BAD_REQUEST, "Unknown");
        }
    }
    su_close(fd);
    // Auto-release any remaining locks owned by this connection
    unlock_all_for_fd(fd);
    free(ci);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <nm_host> <nm_port> <ss_port> <storage_dir>\n", argv[0]);
        return 1;
    }
    snprintf(G_NM_HOST, sizeof(G_NM_HOST), "%s", argv[1]);
    G_NM_PORT = (uint16_t)atoi(argv[2]);
    G_SS_PORT = (uint16_t)atoi(argv[3]);
    const char *root = argv[4];

    log_init("ss.log");
    G_ST = ss_state_load(root);

    // Register to NM (robust retry)
    register_with_nm_retry(G_NM_HOST, G_NM_PORT, G_SS_PORT);

    int sfd = su_listen(G_SS_PORT, BACKLOG);
    if (sfd < 0) { perror("listen"); return 1; }
    log_info("SS listening on %u", G_SS_PORT);

    while (1) {
        uint16_t pp; char ip[64];
        int cfd = su_accept(sfd, ip, sizeof(ip), &pp);
        if (cfd < 0) { if (errno==EINTR) continue; perror("accept"); break; }
        SSConn *ci = (SSConn*)calloc(1, sizeof(SSConn));
        ci->fd = cfd; snprintf(ci->ip, sizeof(ci->ip), "%s", ip); ci->port = pp;
        pthread_t th; pthread_create(&th, NULL, client_thread, ci); pthread_detach(th);
    }

    ss_state_free(G_ST);
    log_close();
    return 0;
}
