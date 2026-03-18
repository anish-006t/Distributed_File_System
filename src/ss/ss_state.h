#ifndef SS_STATE_H
#define SS_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../common/hashmap.h"

// Represents a lock on a specific sentence
typedef struct SentenceLock {
    int sentence_idx;
    char user[128];
    int owner_fd;               // The socket fd that owns this lock (for auto-release)
    struct SentenceLock *next;
} SentenceLock;

// Represents a version/flashcard for UNDO (stack of previous states)
typedef struct UndoFlashcard {
    char *content;           // Backup of file content
    char user[128];          // User who made the change
    time_t timestamp;        // When the change was made
    struct UndoFlashcard *next;  // Next older version
} UndoFlashcard;

typedef struct FileRec {
    char *name;
    char *path;      // content path
    char *prev_path; // undo previous version (deprecated, keeping for compatibility)
    SentenceLock *locks;  // Linked list of sentence locks
    UndoFlashcard *undo_stack;  // Stack of previous versions for UNDO
} FileRec;

typedef struct SSState {
    char root[512];
    HashMap *files; // name -> FileRec*
} SSState;

SSState *ss_state_load(const char *root);
void ss_state_free(SSState *st);
FileRec *ss_get_or_create_file(SSState *st, const char *name);
FileRec *ss_get_file(SSState *st, const char *name);
int ss_delete_file(SSState *st, const char *name);
char **ss_list_all_files(SSState *st, int *count);  // List all files for NM sync

#endif // SS_STATE_H
