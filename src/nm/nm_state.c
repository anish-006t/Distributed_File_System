#include "nm_state.h"
#include "../common/file_utils.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void free_storage_server_ref(StorageServerRef *ssr) {
    while (ssr) {
        StorageServerRef *next = ssr->next;
        free(ssr->host);
        free(ssr);
        ssr = next;
    }
}

static void free_filemeta(void *p) {
    if (!p) return;
    FileMeta *f = (FileMeta*)p;
    Access *a = f->acl;
    while (a) { Access *n = a->next; free(a->user); free(a); a = n; }
    SentenceLock *lock = f->locks;
    while (lock) { SentenceLock *n = lock->next; free(lock); lock = n; }
    free(f->name);
    free(f->owner);
    if (f->last_accessor) { free(f->last_accessor); f->last_accessor = NULL; }
    free_storage_server_ref(f->storage_servers);
    free(f);
}

NMState *nm_state_load(const char *data_dir) {
    NMState *st = (NMState*)calloc(1, sizeof(NMState));
    snprintf(st->data_dir, sizeof(st->data_dir), "%s", data_dir);
    fu_mkdirs(data_dir);
    st->files = hm_create(2048);
    st->users = hm_create(1024);
    st->active_sessions = NULL;
    st->all_users = NULL;
    st->ss_list = NULL;
    st->next_ss_id = 1;
    pthread_mutex_init(&st->state_mutex, NULL);

    // Enhanced persistence: load all file metadata including ACL and timestamps
    char idx[1024]; snprintf(idx, sizeof(idx), "%s/index.tsv", data_dir);
    size_t len; char *data = fu_read_all(idx, &len);
    if (!data) return st; // empty

    char *line = strtok(data, "\n");
    while (line) {
        // Enhanced format: name|owner|words|chars|created|modified|last_access|last_accessor|servers|acl
        // Fall back to old format if needed: name\towner\tservers
        if (strchr(line, '|')) {
            // New enhanced format
            char *parts[10];
            int part_count = 0;
            char *token = strtok(line, "|");
            while (token && part_count < 10) {
                parts[part_count++] = token;
                token = strtok(NULL, "|");
            }
            
            if (part_count >= 9) {
                FileMeta *fm = (FileMeta*)calloc(1, sizeof(FileMeta));
                fm->name = strdup(parts[0]);
                fm->owner = strdup(parts[1]);
                fm->words = (size_t)atoll(parts[2]);
                fm->chars = (size_t)atoll(parts[3]);
                fm->created = (time_t)atol(parts[4]);
                fm->modified = (time_t)atol(parts[5]);
                fm->last_access = (time_t)atol(parts[6]);
                if (strlen(parts[7]) > 0) fm->last_accessor = strdup(parts[7]);
                
                // Parse storage servers
                char *server_token = strtok(parts[8], ",");
                StorageServerRef *prev_ssr = NULL;
                while (server_token) {
                    char host[256]; int port;
                    if (sscanf(server_token, "%255[^:]:%d", host, &port) == 2) {
                        StorageServerRef *ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                        ssr->host = strdup(host);
                        ssr->port = (uint16_t)port;
                        if (!fm->storage_servers) {
                            fm->storage_servers = ssr;
                        } else {
                            prev_ssr->next = ssr;
                        }
                        prev_ssr = ssr;
                    }
                    server_token = strtok(NULL, ",");
                }
                
                // Parse ACL
                if (part_count > 9 && strlen(parts[9]) > 0) {
                    char *acl_token = strtok(parts[9], ",");
                    while (acl_token) {
                        char user[256]; char perm;
                        if (sscanf(acl_token, "%255[^:]:%c", user, &perm) == 2) {
                            nm_acl_grant(fm, user, perm == 'w');
                        }
                        acl_token = strtok(NULL, ",");
                    }
                } else {
                    // Grant owner RW by default if no ACL
                    nm_acl_grant(fm, fm->owner, true);
                }
                
                hm_put(st->files, fm->name, fm, NULL);
            }
        } else {
            // Old format fallback: name\towner\tservers
            char name[256], owner[256], servers[1024];
            if (sscanf(line, "%255[^\t]\t%255[^\t]\t%1023[^\t\n]", name, owner, servers) == 3) {
                FileMeta *fm = (FileMeta*)calloc(1, sizeof(FileMeta));
                fm->name = strdup(name);
                fm->owner = strdup(owner);
                fm->created = fm->modified = fm->last_access = time(NULL);
                
                // Parse storage servers
                char *server_token = strtok(servers, ",");
                StorageServerRef *prev_ssr = NULL;
                while (server_token) {
                    char host[256]; int port;
                    if (sscanf(server_token, "%255[^:]:%d", host, &port) == 2) {
                        StorageServerRef *ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
                        ssr->host = strdup(host);
                        ssr->port = (uint16_t)port;
                        if (!fm->storage_servers) {
                            fm->storage_servers = ssr;
                        } else {
                            prev_ssr->next = ssr;
                        }
                        prev_ssr = ssr;
                    }
                    server_token = strtok(NULL, ",");
                }
                
                hm_put(st->files, fm->name, fm, NULL);
                // grant owner RW by default
                nm_acl_grant(fm, owner, true);
            }
        }
        line = strtok(NULL, "\n");
    }
    free(data);
    
    // Load user sessions
    char sessions_file[1024]; snprintf(sessions_file, sizeof(sessions_file), "%s/sessions.dat", data_dir);
    size_t sessions_len;
    char *sessions_data = fu_read_all(sessions_file, &sessions_len);
    if (sessions_data) {
        char *session_line = strtok(sessions_data, "\n");
        while (session_line) {
            char username[256];
            long login_time, last_activity;
            int is_active;
            if (sscanf(session_line, "%255[^|]|%ld|%ld|%d", username, &login_time, &last_activity, &is_active) == 4) {
                UserSession *user_session = (UserSession*)calloc(1, sizeof(UserSession));
                user_session->username = strdup(username);
                user_session->login_time = (time_t)login_time;
                user_session->last_activity = (time_t)last_activity;
                user_session->is_active = is_active ? true : false;
                user_session->next = st->all_users;
                st->all_users = user_session;
                
                // Add to active sessions if still active (though usually they won't be after restart)
                if (is_active) {
                    UserSession *active_session = (UserSession*)calloc(1, sizeof(UserSession));
                    active_session->username = strdup(username);
                    active_session->login_time = user_session->login_time;
                    active_session->last_activity = user_session->last_activity;
                    active_session->is_active = true;
                    active_session->next = st->active_sessions;
                    st->active_sessions = active_session;
                }
            }
            session_line = strtok(NULL, "\n");
        }
        free(sessions_data);
    }
    return st;
}

void nm_state_save(NMState *st) {
    char idx[1024]; snprintf(idx, sizeof(idx), "%s/index.tsv", st->data_dir);
    FILE *f = fopen(idx, "wb"); if (!f) return;
    
    // Enhanced format: name|owner|words|chars|created|modified|last_access|last_accessor|servers|acl
    for (size_t i = 0; i < st->files->nbuckets; ++i) {
        HMEntry *e = st->files->buckets[i];
        while (e) {
            FileMeta *fm = (FileMeta*)e->value;
            fprintf(f, "%s|%s|%zu|%zu|%ld|%ld|%ld|%s|", 
                   fm->name, fm->owner, fm->words, fm->chars,
                   (long)fm->created, (long)fm->modified, (long)fm->last_access,
                   fm->last_accessor ? fm->last_accessor : "");
            
            // Write storage servers as comma-separated list
            StorageServerRef *ssr = fm->storage_servers;
            bool first = true;
            while (ssr) {
                if (!first) fprintf(f, ",");
                fprintf(f, "%s:%u", ssr->host, ssr->port);
                first = false;
                ssr = ssr->next;
            }
            fprintf(f, "|");
            
            // Write ACL as user1:w,user2:r format
            Access *acl = fm->acl;
            first = true;
            while (acl) {
                if (!first) fprintf(f, ",");
                fprintf(f, "%s:%c", acl->user, acl->can_write ? 'w' : 'r');
                first = false;
                acl = acl->next;
            }
            fprintf(f, "\n");
            e = e->next;
        }
    }
    fclose(f);
    
    // Save user sessions
    char sessions_file[1024]; snprintf(sessions_file, sizeof(sessions_file), "%s/sessions.dat", st->data_dir);
    FILE *sf = fopen(sessions_file, "wb");
    if (sf) {
        UserSession *user = st->all_users;
        while (user) {
            fprintf(sf, "%s|%ld|%ld|%d\n", user->username, 
                   (long)user->login_time, (long)user->last_activity, user->is_active ? 1 : 0);
            user = user->next;
        }
        fclose(sf);
    }
}

void nm_state_free(NMState *st) {
    if (!st) return;
    hm_free(st->files, free_filemeta);
    hm_free(st->users, NULL);
    
    // Free user sessions
    UserSession *session = st->active_sessions;
    while (session) { 
        UserSession *next = session->next; 
        free(session->username); 
        free(session); 
        session = next; 
    }
    
    session = st->all_users;
    while (session) { 
        UserSession *next = session->next; 
        free(session->username); 
        free(session); 
        session = next; 
    }
    
    StorageServer *p = st->ss_list;
    while (p) { StorageServer *n = p->next; free(p->host); free(p); p = n; }
    pthread_mutex_destroy(&st->state_mutex);
    free(st);
}

FileMeta *nm_get_file(NMState *st, const char *name) {
    return (FileMeta*)hm_get(st->files, name);
}

FileMeta *nm_create_file(NMState *st, const char *name, const char *owner) {
    FileMeta *fm = (FileMeta*)calloc(1, sizeof(FileMeta));
    fm->name = strdup(name);
    fm->owner = strdup(owner);
    fm->created = fm->modified = fm->last_access = time(NULL);
    fm->locks = NULL; // Initialize centralized locks
    
    // Pick 3 storage servers for this file
    fm->storage_servers = nm_pick_storage_servers(st, 3);
    
    nm_acl_grant(fm, owner, true);
    hm_put(st->files, fm->name, fm, NULL);
    nm_state_save(st);
    return fm;
}

bool nm_delete_file(NMState *st, const char *name) {
    void *oldv = NULL;
    bool ok = hm_remove(st->files, name, &oldv);
    if (ok && oldv) free_filemeta(oldv);
    if (ok) nm_state_save(st);
    return ok;
}

static Access *find_access(FileMeta *fm, const char *user) {
    for (Access *a = fm->acl; a; a = a->next) {
        if (strcmp(a->user, user) == 0) return a;
    }
    return NULL;
}

bool nm_user_has_read(FileMeta *fm, const char *user) {
    if (!fm || !user) return false;
    if (strcmp(fm->owner, user) == 0) return true;
    Access *a = find_access(fm, user);
    return a != NULL; // read if present
}

bool nm_user_has_write(FileMeta *fm, const char *user) {
    if (!fm || !user) return false;
    if (strcmp(fm->owner, user) == 0) return true;
    Access *a = find_access(fm, user);
    return a && a->can_write;
}

void nm_acl_grant(FileMeta *fm, const char *user, bool write) {
    Access *a = find_access(fm, user);
    if (!a) {
        a = (Access*)calloc(1, sizeof(Access));
        a->user = strdup(user);
        a->next = fm->acl;
        fm->acl = a;
    }
    if (write) a->can_write = true;
}

void nm_acl_revoke(FileMeta *fm, const char *user) {
    Access *prev = NULL; Access *a = fm->acl;
    while (a) {
        if (strcmp(a->user, user) == 0) {
            if (prev) prev->next = a->next; else fm->acl = a->next;
            free(a->user); free(a); return;
        }
        prev = a; a = a->next;
    }
}

void nm_register_user(NMState *st, const char *user) {
    hm_put(st->users, user, (void*)1, NULL);
}

bool nm_user_exists(NMState *st, const char *user) {
    return hm_get(st->users, user) != NULL;
}

void nm_add_ss(NMState *st, const char *host, uint16_t port) {
    StorageServer *ss = (StorageServer*)calloc(1, sizeof(StorageServer));
    ss->host = strdup(host);
    ss->port = port;
    ss->id = st->next_ss_id++;
    ss->healthy = false; // unknown until first check
    ss->fail_count = 0;
    ss->last_check = 0;
    ss->last_ok = 0;
    ss->next = st->ss_list;
    st->ss_list = ss;
}

StorageServerRef *nm_pick_storage_servers(NMState *st, int count) {
    if (!st->ss_list || count <= 0) return NULL;
    
    // Count available storage servers
    int total_servers = 0, healthy_servers = 0;
    StorageServer *s = st->ss_list;
    while (s) { total_servers++; if (s->healthy) healthy_servers++; s = s->next; }
    
    if (total_servers == 0) return NULL;
    
    // Pick up to 'count' servers (or all available if less than count)
    int servers_to_pick = (count < total_servers) ? count : total_servers;
    
    // Build selection pool: prefer healthy servers; fallback to all if none healthy
    int pool_size = (healthy_servers > 0) ? healthy_servers : total_servers;
    StorageServer **servers = malloc(pool_size * sizeof(StorageServer*));
    s = st->ss_list; int idx = 0;
    if (healthy_servers > 0) {
        while (s) { if (s->healthy) servers[idx++] = s; s = s->next; }
    } else {
        while (s) { servers[idx++] = s; s = s->next; }
    }
    
    // Shuffle and pick first 'servers_to_pick' servers
    srand(time(NULL));
    for (int i = 0; i < pool_size - 1; i++) {
        int j = i + rand() % (pool_size - i);
        StorageServer *temp = servers[i];
        servers[i] = servers[j];
        servers[j] = temp;
    }
    
    // Create linked list of selected servers
    StorageServerRef *head = NULL, *prev = NULL;
    for (int i = 0; i < servers_to_pick && i < pool_size; i++) {
        StorageServerRef *ssr = (StorageServerRef*)calloc(1, sizeof(StorageServerRef));
        ssr->host = strdup(servers[i]->host);
        ssr->port = servers[i]->port;
        ssr->id = servers[i]->id;
        
        if (!head) {
            head = ssr;
        } else {
            prev->next = ssr;
        }
        prev = ssr;
    }
    
    free(servers);
    return head;
}

StorageServerRef *nm_get_available_server(FileMeta *fm) {
    // Prefer healthy servers by id order, otherwise fallback to first
    if (!fm || !fm->storage_servers) return NULL;
    // No direct access to health here; main.c provides a smarter helper. Fallback for now.
    return fm->storage_servers;
}

StorageServer *nm_pick_ss(NMState *st) {
    // Legacy function - return first available server
    return st->ss_list;
}

// User session management functions
void nm_user_login(NMState *st, const char *username) {
    if (!st || !username) return;
    
    time_t now = time(NULL);
    
    // Check if user already has an active session
    UserSession *session = st->active_sessions;
    while (session) {
        if (strcmp(session->username, username) == 0) {
            session->last_activity = now;
            session->is_active = true;
            return;
        }
        session = session->next;
    }
    
    // Create new active session
    UserSession *new_session = (UserSession*)calloc(1, sizeof(UserSession));
    new_session->username = strdup(username);
    new_session->login_time = now;
    new_session->last_activity = now;
    new_session->is_active = true;
    new_session->next = st->active_sessions;
    st->active_sessions = new_session;
    
    // Add to all users list if not already there
    UserSession *all_user = st->all_users;
    while (all_user) {
        if (strcmp(all_user->username, username) == 0) {
            all_user->is_active = true;
            all_user->last_activity = now;
            return; // Already in all users list
        }
        all_user = all_user->next;
    }
    
    // Add to all users list
    UserSession *new_all_user = (UserSession*)calloc(1, sizeof(UserSession));
    new_all_user->username = strdup(username);
    new_all_user->login_time = now;
    new_all_user->last_activity = now;
    new_all_user->is_active = true;
    new_all_user->next = st->all_users;
    st->all_users = new_all_user;
}

void nm_user_logout(NMState *st, const char *username) {
    if (!st || !username) return;
    
    UserSession *session = st->active_sessions;
    UserSession *prev = NULL;
    
    while (session) {
        if (strcmp(session->username, username) == 0) {
            session->is_active = false;
            // Remove from active sessions
            if (prev) {
                prev->next = session->next;
            } else {
                st->active_sessions = session->next;
            }
            
            // Update in all users list
            UserSession *all_user = st->all_users;
            while (all_user) {
                if (strcmp(all_user->username, username) == 0) {
                    all_user->is_active = false;
                    break;
                }
                all_user = all_user->next;
            }
            
            free(session->username);
            free(session);
            return;
        }
        prev = session;
        session = session->next;
    }
}

void nm_user_update_activity(NMState *st, const char *username) {
    if (!st || !username) return;
    
    time_t now = time(NULL);
    
    // Update active session
    UserSession *session = st->active_sessions;
    while (session) {
        if (strcmp(session->username, username) == 0) {
            session->last_activity = now;
            break;
        }
        session = session->next;
    }
    
    // Update all users list
    UserSession *all_user = st->all_users;
    while (all_user) {
        if (strcmp(all_user->username, username) == 0) {
            all_user->last_activity = now;
            break;
        }
        all_user = all_user->next;
    }
}

UserSession *nm_get_active_users(NMState *st) {
    return st ? st->active_sessions : NULL;
}

UserSession *nm_get_all_users(NMState *st) {
    return st ? st->all_users : NULL;
}

// Centralized sentence locking (NM-level) - caller must hold state_mutex
bool nm_is_sentence_locked(FileMeta *fm, int sentence_idx, const char *user) {
    if (!fm) return false;
    for (SentenceLock *lock = fm->locks; lock; lock = lock->next) {
        if (lock->sentence_idx == sentence_idx) {
            // Locked - check if by same user
            return strcmp(lock->user, user) != 0; // TRUE if different user
        }
    }
    return false; // Not locked
}

bool nm_lock_sentence(FileMeta *fm, int sentence_idx, const char *user) {
    if (!fm) return false;
    
    // Check if already locked by someone else
    for (SentenceLock *lock = fm->locks; lock; lock = lock->next) {
        if (lock->sentence_idx == sentence_idx) {
            if (strcmp(lock->user, user) == 0) {
                return true; // Already locked by same user - allow
            }
            return false; // Locked by different user
        }
    }
    
    // Not locked - acquire lock
    SentenceLock *new_lock = (SentenceLock*)malloc(sizeof(SentenceLock));
    new_lock->sentence_idx = sentence_idx;
    snprintf(new_lock->user, sizeof(new_lock->user), "%s", user);
    new_lock->lock_time = time(NULL);
    new_lock->next = fm->locks;
    fm->locks = new_lock;
    
    log_info("NM LOCK ACQUIRED: file=%s sentence=%d user=%s", fm->name, sentence_idx, user);
    return true;
}

void nm_unlock_sentence(FileMeta *fm, int sentence_idx, const char *user) {
    if (!fm) return;
    
    SentenceLock *prev = NULL;
    SentenceLock *curr = fm->locks;
    while (curr) {
        if (curr->sentence_idx == sentence_idx && strcmp(curr->user, user) == 0) {
            if (prev) {
                prev->next = curr->next;
            } else {
                fm->locks = curr->next;
            }
            log_info("NM LOCK RELEASED: file=%s sentence=%d user=%s", fm->name, sentence_idx, user);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void nm_unlock_all_sentences(FileMeta *fm, const char *user) {
    if (!fm) return;
    
    SentenceLock *prev = NULL;
    SentenceLock *curr = fm->locks;
    while (curr) {
        if (strcmp(curr->user, user) == 0) {
            SentenceLock *to_free = curr;
            curr = curr->next;
            if (prev) {
                prev->next = curr;
            } else {
                fm->locks = curr;
            }
            log_info("NM LOCK RELEASED (cleanup): file=%s sentence=%d user=%s", 
                     fm->name, to_free->sentence_idx, user);
            free(to_free);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}
