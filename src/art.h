#ifndef RTREE_ART_H
#define RTREE_ART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ART_LIMIT_UNLIMITED ((size_t)-1)

typedef struct ArtTree ArtTree;

typedef void *(*ArtAllocFn)(size_t size);
typedef void *(*ArtCallocFn)(size_t nmemb, size_t size);
typedef void *(*ArtReallocFn)(void *ptr, size_t size);
typedef void (*ArtFreeFn)(void *ptr);
typedef void (*ArtValueFreeFn)(void *value);
typedef uint64_t (*ArtValueMemUsageFn)(void *value);
typedef void *(*ArtDefragAllocFn)(void *ctx, void *ptr);
typedef void *(*ArtValueDefragFn)(void *ctx, void *value);
typedef int (*ArtIterFn)(const unsigned char *key, size_t key_len, void *value, void *ctx);

void art_set_allocator(ArtAllocFn alloc_fn,
                       ArtCallocFn calloc_fn,
                       ArtReallocFn realloc_fn,
                       ArtFreeFn free_fn);

ArtTree *art_create(ArtValueFreeFn free_value);
void art_destroy(ArtTree *tree);

uint64_t art_size(const ArtTree *tree);
uint64_t art_memory_usage(const ArtTree *tree, ArtValueMemUsageFn value_mem_usage);
uint64_t art_free_effort(const ArtTree *tree);
ArtTree *art_defrag(ArtTree *tree,
                    ArtDefragAllocFn defrag_alloc,
                    ArtValueDefragFn defrag_value,
                    void *ctx);

int art_insert(ArtTree *tree,
               const unsigned char *key,
               size_t key_len,
               void *value,
               void **old_value);

void *art_search(const ArtTree *tree, const unsigned char *key, size_t key_len);

int art_delete(ArtTree *tree,
               const unsigned char *key,
               size_t key_len,
               void **old_value);

int art_iter(const ArtTree *tree, ArtIterFn callback, void *ctx);
int art_reverse_iter(const ArtTree *tree, ArtIterFn callback, void *ctx);

int art_range(const ArtTree *tree,
              const unsigned char *start,
              size_t start_len,
              const unsigned char *end,
              size_t end_len,
              int reverse,
              size_t limit,
              ArtIterFn callback,
              void *ctx);

int art_prefix(const ArtTree *tree,
               const unsigned char *prefix,
               size_t prefix_len,
               size_t limit,
               ArtIterFn callback,
               void *ctx);

#ifdef __cplusplus
}
#endif

#endif
