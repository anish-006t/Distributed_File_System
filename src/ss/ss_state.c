#include "ss_state.h"
#include "../common/file_utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

static void free_frec(void *p) {
    FileRec *fr = (FileRec*)p;
    if (!fr) return;
    // Free all locks
    SentenceLock *lock = fr->locks;
    while (lock) {
        SentenceLock *next = lock->next;
        free(lock);
        lock = next;
    }
    // Free all undo flashcards
    UndoFlashcard *undo = fr->undo_stack;
    while (undo) {
        UndoFlashcard *next = undo->next;
        free(undo->content);
        free(undo);
        undo = next;
    }
    free(fr->name);
    free(fr->path);
    free(fr->prev_path);
    free(fr);
}

SSState *ss_state_load(const char *root) {
    SSState *st = (SSState*)calloc(1, sizeof(SSState));
    snprintf(st->root, sizeof(st->root), "%s", root);
    char files_dir[1024]; snprintf(files_dir, sizeof(files_dir), "%s/files", root);
    fu_mkdirs(files_dir);
    st->files = hm_create(1024);
    
    // Discover existing files on disk and load them into state
    DIR *dir = opendir(files_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            // Skip . and .. and hidden files (undo backups)
            if (entry->d_name[0] == '.') continue;
            
            // Check if it's a regular file
            char fullpath[2048];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", files_dir, entry->d_name);
            struct stat st_buf;
            if (stat(fullpath, &st_buf) == 0 && S_ISREG(st_buf.st_mode)) {
                // Create FileRec entry for this existing file
                FileRec *fr = (FileRec*)calloc(1, sizeof(FileRec));
                fr->name = strdup(entry->d_name);
                fr->path = strdup(fullpath);
                char prev[2048];
                snprintf(prev, sizeof(prev), "%s/.%s.prev", files_dir, entry->d_name);
                fr->prev_path = strdup(prev);
                fr->locks = NULL;
                fr->undo_stack = NULL;
                hm_put(st->files, fr->name, fr, NULL);
            }
        }
        closedir(dir);
    }
    
    return st;
}

void ss_state_free(SSState *st) {
    hm_free(st->files, free_frec);
    free(st);
}

FileRec *ss_get_or_create_file(SSState *st, const char *name) {
    FileRec *fr = (FileRec*)hm_get(st->files, name);
    if (fr) return fr;
    fr = (FileRec*)calloc(1, sizeof(FileRec));
    fr->name = strdup(name);
    char p[1024]; snprintf(p, sizeof(p), "%s/files/%s", st->root, name);
    fr->path = strdup(p);
    char q[1024]; snprintf(q, sizeof(q), "%s/files/.%s.prev", st->root, name);
    fr->prev_path = strdup(q);
    fr->locks = NULL;  // Initialize lock list as empty
    fr->undo_stack = NULL;  // Initialize undo stack as empty
    hm_put(st->files, fr->name, fr, NULL);
    if (!fu_exists(fr->path)) fu_write_all(fr->path, "", 0);
    return fr;
}

FileRec *ss_get_file(SSState *st, const char *name) {
    return (FileRec*)hm_get(st->files, name);
}

int ss_delete_file(SSState *st, const char *name) {
    void *oldv = NULL;
    if (!hm_remove(st->files, name, &oldv)) return -1;
    FileRec *fr = (FileRec*)oldv;
    // remove files from disk
    remove(fr->path);
    remove(fr->prev_path);
    free_frec(fr);
    return 0;
}

char **ss_list_all_files(SSState *st, int *count) {
    // Count files first
    int n = 0;
    for (size_t i = 0; i < st->files->nbuckets; i++) {
        HMEntry *e = st->files->buckets[i];
        while (e) { n++; e = e->next; }
    }
    
    if (n == 0) {
        *count = 0;
        return NULL;
    }
    
    // Allocate array
    char **list = (char**)malloc(n * sizeof(char*));
    int idx = 0;
    
    // Collect file names
    for (size_t i = 0; i < st->files->nbuckets; i++) {
        HMEntry *e = st->files->buckets[i];
        while (e) {
            FileRec *fr = (FileRec*)e->value;
            list[idx++] = strdup(fr->name);
            e = e->next;
        }
    }
    
    *count = n;
    return list;
}
