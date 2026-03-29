#ifndef MATCHBOX_HASHMAP_H
#define MATCHBOX_HASHMAP_H
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t  key;
    void     *value;
    int       used;   /* 0 = empty, 1 = occupied, -1 = tombstone */
} hm_slot_t;

typedef struct {
    hm_slot_t *slots;
    size_t     cap;
    size_t     count;
} hashmap_t;

void     hm_init(hashmap_t *m, size_t initial_cap);  /* cap must be power of 2 */
void     hm_free(hashmap_t *m);
void    *hm_get(hashmap_t *m, uint64_t key);
void     hm_put(hashmap_t *m, uint64_t key, void *value);
void     hm_delete(hashmap_t *m, uint64_t key);

#endif /* MATCHBOX_HASHMAP_H */
