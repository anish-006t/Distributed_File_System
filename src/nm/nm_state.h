#ifndef NM_STATE_H
#define NM_STATE_H

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include "../common/hashmap.h"

typedef struct Access {
    char *user;
    bool can_write; // implies read
    struct Access *next;
} Access;

typedef struct StorageServerRef {
    char *host;
    uint16_t port;
    int id;
    struct StorageServerRef *next;
} StorageServerRef;

// Centralized sentence lock (managed by NM)
typedef struct SentenceLock {
    int sentence_idx;
    char user[128];
    time_t lock_time;
    struct SentenceLock *next;
} SentenceLock;

typedef struct FileMeta {
    char *name;
    char *owner;
    size_t words;
    size_t chars;
    time_t created;
    time_t modified;
    time_t last_access;
    char *last_accessor;
    StorageServerRef *storage_servers; // List of 3 storage servers for this file
    Access *acl;
    SentenceLock *locks; // Centralized sentence locks (NM-level)
} FileMeta;

typedef struct StorageServer {
    char *host;
    uint16_t port;
    int id;
    // Health info (managed by NM):
    bool healthy;           // last known health
    int fail_count;         // consecutive failures
    time_t last_check;      // last time we checked
    time_t last_ok;         // last time it was healthy
    struct StorageServer *next;
} StorageServer;

typedef struct UserSession {
    char *username;
    time_t login_time;
    time_t last_activity;
    bool is_active;
    struct UserSession *next;
} UserSession;

typedef struct NMState {
    HashMap *files; // key: filename -> FileMeta*
    HashMap *users; // username -> (void*)1
    UserSession *active_sessions; // Linked list of active user sessions
    UserSession *all_users; // Linked list of all users who have ever logged in
    StorageServer *ss_list;
    int next_ss_id;
    char data_dir[512];
    pthread_mutex_t state_mutex; // protects files/users/meta modifications & persistence
} NMState;

NMState *nm_state_load(const char *data_dir);
void nm_state_save(NMState *st);
void nm_state_free(NMState *st);

FileMeta *nm_get_file(NMState *st, const char *name);
FileMeta *nm_create_file(NMState *st, const char *name, const char *owner);
StorageServerRef *nm_pick_storage_servers(NMState *st, int count);
StorageServerRef *nm_get_available_server(FileMeta *fm);
bool nm_delete_file(NMState *st, const char *name);

bool nm_user_has_read(FileMeta *fm, const char *user);
bool nm_user_has_write(FileMeta *fm, const char *user);
void nm_acl_grant(FileMeta *fm, const char *user, bool write);
void nm_acl_revoke(FileMeta *fm, const char *user);

// Centralized sentence locking functions (NM-level, must hold state_mutex)
bool nm_is_sentence_locked(FileMeta *fm, int sentence_idx, const char *user);
bool nm_lock_sentence(FileMeta *fm, int sentence_idx, const char *user);
void nm_unlock_sentence(FileMeta *fm, int sentence_idx, const char *user);
void nm_unlock_all_sentences(FileMeta *fm, const char *user);

void nm_register_user(NMState *st, const char *user);
bool nm_user_exists(NMState *st, const char *user);
void nm_add_ss(NMState *st, const char *host, uint16_t port);
StorageServer *nm_pick_ss(NMState *st);

// User session management
void nm_user_login(NMState *st, const char *username);
void nm_user_logout(NMState *st, const char *username);
void nm_user_update_activity(NMState *st, const char *username);
UserSession *nm_get_active_users(NMState *st);
UserSession *nm_get_all_users(NMState *st);

#endif // NM_STATE_H
