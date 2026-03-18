#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>
#include <stdbool.h>

typedef struct HMEntry {
    char *key;
    void *value;
    struct HMEntry *next;
} HMEntry;

typedef struct HashMap {
    HMEntry **buckets;
    size_t nbuckets;
    size_t size;
} HashMap;

HashMap *hm_create(size_t nbuckets);
void hm_free(HashMap *hm, void (*free_value)(void*));
bool hm_put(HashMap *hm, const char *key, void *value, void **old_value);
void *hm_get(HashMap *hm, const char *key);
bool hm_remove(HashMap *hm, const char *key, void **old_value);

#endif // HASHMAP_H
