#include "hashmap.h"

#include <stdlib.h>
#include <string.h>

static unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) hash = ((hash << 5) + hash) + c;
    return hash;
}

HashMap *hm_create(size_t nbuckets) {
    HashMap *hm = (HashMap*)calloc(1, sizeof(HashMap));
    hm->nbuckets = nbuckets ? nbuckets : 1024;
    hm->buckets = (HMEntry**)calloc(hm->nbuckets, sizeof(HMEntry*));
    return hm;
}

void hm_free(HashMap *hm, void (*free_value)(void*)) {
    if (!hm) return;
    for (size_t i = 0; i < hm->nbuckets; ++i) {
        HMEntry *e = hm->buckets[i];
        while (e) {
            HMEntry *n = e->next;
            free(e->key);
            if (free_value) free_value(e->value);
            free(e);
            e = n;
        }
    }
    free(hm->buckets);
    free(hm);
}

bool hm_put(HashMap *hm, const char *key, void *value, void **old_value) {
    unsigned long h = djb2(key) % hm->nbuckets;
    HMEntry *e = hm->buckets[h];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            if (old_value) *old_value = e->value;
            e->value = value;
            return false; // replaced
        }
        e = e->next;
    }
    HMEntry *ne = (HMEntry*)calloc(1, sizeof(HMEntry));
    ne->key = strdup(key);
    ne->value = value;
    ne->next = hm->buckets[h];
    hm->buckets[h] = ne;
    hm->size++;
    if (old_value) *old_value = NULL;
    return true;
}

void *hm_get(HashMap *hm, const char *key) {
    unsigned long h = djb2(key) % hm->nbuckets;
    HMEntry *e = hm->buckets[h];
    while (e) {
        if (strcmp(e->key, key) == 0) return e->value;
        e = e->next;
    }
    return NULL;
}

bool hm_remove(HashMap *hm, const char *key, void **old_value) {
    unsigned long h = djb2(key) % hm->nbuckets;
    HMEntry *prev = NULL;
    HMEntry *e = hm->buckets[h];
    while (e) {
        if (strcmp(e->key, key) == 0) {
            if (prev) prev->next = e->next; else hm->buckets[h] = e->next;
            if (old_value) *old_value = e->value; else if (e->value) free(e->value);
            free(e->key);
            free(e);
            hm->size--;
            return true;
        }
        prev = e;
        e = e->next;
    }
    return false;
}
