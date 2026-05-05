#include "art.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ART_NODE4 = 4,
    ART_NODE16 = 16,
    ART_NODE48 = 48,
    ART_NODE256 = 256
} ArtNodeType;

#define ART_INLINE_PREFIX_SIZE (sizeof(void *))
#define ART_SLAB_SIZE 65536

typedef struct ArtNode ArtNode;

typedef struct {
    unsigned char keys[4];
    ArtNode *children[4];
} ArtNode4;

typedef struct {
    unsigned char keys[16];
    ArtNode *children[16];
} ArtNode16;

typedef struct {
    unsigned char child_index[256];
    ArtNode *children[48];
} ArtNode48;

typedef struct {
    ArtNode *children[256];
} ArtNode256;

typedef union {
    unsigned char *heap;
    unsigned char inlined[ART_INLINE_PREFIX_SIZE];
} ArtPrefix;

struct ArtNode {
    ArtNodeType type;
    uint16_t count;
    unsigned char has_value;
    size_t prefix_len;
    ArtPrefix prefix;
    void *value;
};

typedef struct {
    ArtNode base;
    ArtNode4 data;
} ArtNode4Object;

typedef struct {
    ArtNode base;
    ArtNode16 data;
} ArtNode16Object;

typedef struct {
    ArtNode base;
    ArtNode48 data;
} ArtNode48Object;

typedef struct {
    ArtNode base;
    ArtNode256 data;
} ArtNode256Object;

typedef struct ArtFreeNode {
    struct ArtFreeNode *next;
} ArtFreeNode;

typedef struct ArtSlab {
    struct ArtSlab *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
} ArtSlab;

typedef struct {
    ArtFreeNode *freelist;
    ArtSlab *slabs;
    size_t object_size;
    size_t allocated;
} ArtPool;

struct ArtTree {
    ArtNode *root;
    uint64_t size;
    ArtValueFreeFn free_value;
    ArtPool pools[4];
};

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} ArtKeyBuffer;

typedef struct {
    const unsigned char *start;
    size_t start_len;
    const unsigned char *end;
    size_t end_len;
    size_t limit;
    size_t emitted;
    ArtIterFn callback;
    void *ctx;
} ArtRangeCtx;

typedef struct {
    const unsigned char *prefix;
    size_t prefix_len;
    size_t limit;
    size_t emitted;
    ArtIterFn callback;
    void *ctx;
} ArtPrefixCtx;

typedef struct {
    const unsigned char *prefix;
    size_t prefix_len;
} ArtPrefixFilter;

typedef struct {
    const unsigned char *start;
    size_t start_len;
    const unsigned char *end;
    size_t end_len;
} ArtRangeFilter;

typedef struct {
    ArtNode *node;
    size_t base_len;
    int child_idx;
    unsigned char edge;
    unsigned char has_edge;
} TraverseFrame;

typedef struct {
    TraverseFrame *items;
    size_t len;
    size_t cap;
} TraverseStack;

#define TRAVERSE_ENTER (-1)
#define TRAVERSE_DONE (-2)
#define RANGE_SUBTREE_OUT 0
#define RANGE_SUBTREE_IN 1
#define RANGE_VALUE_ONLY 2
#define RANGE_CHILD_SKIP 0
#define RANGE_CHILD_VISIT 1
#define RANGE_CHILD_STOP 2

static void *default_alloc(size_t size) {
    return malloc(size);
}

static void *default_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

static void *default_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static void default_free(void *ptr) {
    free(ptr);
}

static ArtAllocFn g_alloc = default_alloc;
static ArtCallocFn g_calloc = default_calloc;
static ArtReallocFn g_realloc = default_realloc;
static ArtFreeFn g_free = default_free;
static const unsigned char g_empty_key[] = "";

static size_t align_up_size(size_t value, size_t align) {
    size_t rem;

    if (align == 0) {
        return value;
    }
    rem = value % align;
    return rem == 0 ? value : value + align - rem;
}

static int node_pool_index(ArtNodeType type) {
    switch (type) {
        case ART_NODE4:
            return 0;
        case ART_NODE16:
            return 1;
        case ART_NODE48:
            return 2;
        case ART_NODE256:
            return 3;
    }
    return 0;
}

static size_t node_object_size(ArtNodeType type) {
    switch (type) {
        case ART_NODE4:
            return sizeof(ArtNode4Object);
        case ART_NODE16:
            return sizeof(ArtNode16Object);
        case ART_NODE48:
            return sizeof(ArtNode48Object);
        case ART_NODE256:
            return sizeof(ArtNode256Object);
    }
    return sizeof(ArtNode4Object);
}

static void art_pool_init(ArtPool *pool, size_t object_size) {
    pool->freelist = NULL;
    pool->slabs = NULL;
    pool->object_size = align_up_size(object_size, sizeof(void *));
    pool->allocated = 0;
}

static void *art_pool_alloc(ArtPool *pool) {
    ArtFreeNode *free_node;
    ArtSlab *slab;
    void *ptr;

    if (pool->freelist != NULL) {
        free_node = pool->freelist;
        pool->freelist = free_node->next;
        memset(free_node, 0, pool->object_size);
        return free_node;
    }

    slab = pool->slabs;
    if (slab == NULL || slab->capacity - slab->used < pool->object_size) {
        size_t capacity = ART_SLAB_SIZE;
        size_t bytes;

        if (capacity < pool->object_size) {
            capacity = pool->object_size;
        }
        bytes = sizeof(*slab) + capacity;
        slab = g_alloc(bytes);
        if (slab == NULL) {
            return NULL;
        }
        slab->next = pool->slabs;
        slab->capacity = capacity;
        slab->used = 0;
        pool->slabs = slab;
    }

    ptr = slab->data + slab->used;
    slab->used += pool->object_size;
    pool->allocated++;
    memset(ptr, 0, pool->object_size);
    return ptr;
}

static void art_pool_free(ArtPool *pool, void *ptr) {
    ArtFreeNode *free_node;

    if (ptr == NULL) {
        return;
    }

    free_node = ptr;
    free_node->next = pool->freelist;
    pool->freelist = free_node;
}

static void art_pool_destroy(ArtPool *pool) {
    ArtSlab *slab = pool->slabs;

    while (slab != NULL) {
        ArtSlab *next = slab->next;
        g_free(slab);
        slab = next;
    }

    pool->freelist = NULL;
    pool->slabs = NULL;
    pool->allocated = 0;
}

static uint64_t art_pool_memory_usage(const ArtPool *pool) {
    const ArtSlab *slab;
    uint64_t total = 0;

    for (slab = pool->slabs; slab != NULL; slab = slab->next) {
        total += sizeof(*slab) + slab->capacity;
    }
    return total;
}

static ArtPool *tree_pool_for_type(ArtTree *tree, ArtNodeType type) {
    return &tree->pools[node_pool_index(type)];
}

static ArtNode4 *node4_data(ArtNode *node) {
    return (ArtNode4 *)((unsigned char *)node + offsetof(ArtNode4Object, data));
}

static ArtNode16 *node16_data(ArtNode *node) {
    return (ArtNode16 *)((unsigned char *)node + offsetof(ArtNode16Object, data));
}

static ArtNode48 *node48_data(ArtNode *node) {
    return (ArtNode48 *)((unsigned char *)node + offsetof(ArtNode48Object, data));
}

static ArtNode256 *node256_data(ArtNode *node) {
    return (ArtNode256 *)((unsigned char *)node + offsetof(ArtNode256Object, data));
}

static const ArtNode4 *node4_data_const(const ArtNode *node) {
    return (const ArtNode4 *)((const unsigned char *)node + offsetof(ArtNode4Object, data));
}

static const ArtNode16 *node16_data_const(const ArtNode *node) {
    return (const ArtNode16 *)((const unsigned char *)node + offsetof(ArtNode16Object, data));
}

static const ArtNode48 *node48_data_const(const ArtNode *node) {
    return (const ArtNode48 *)((const unsigned char *)node + offsetof(ArtNode48Object, data));
}

static const ArtNode256 *node256_data_const(const ArtNode *node) {
    return (const ArtNode256 *)((const unsigned char *)node + offsetof(ArtNode256Object, data));
}

static int node_prefix_is_heap(const ArtNode *node) {
    return node->prefix_len > ART_INLINE_PREFIX_SIZE;
}

static const unsigned char *node_prefix_const(const ArtNode *node) {
    return node_prefix_is_heap(node) ? node->prefix.heap : node->prefix.inlined;
}

static void node_release_prefix(ArtNode *node) {
    if (node_prefix_is_heap(node) && node->prefix.heap != NULL) {
        g_free(node->prefix.heap);
    }
    node->prefix.heap = NULL;
    node->prefix_len = 0;
}

void art_set_allocator(ArtAllocFn alloc_fn,
                       ArtCallocFn calloc_fn,
                       ArtReallocFn realloc_fn,
                       ArtFreeFn free_fn) {
    if (alloc_fn == NULL || calloc_fn == NULL || realloc_fn == NULL || free_fn == NULL) {
        g_alloc = default_alloc;
        g_calloc = default_calloc;
        g_realloc = default_realloc;
        g_free = default_free;
        return;
    }

    g_alloc = alloc_fn;
    g_calloc = calloc_fn;
    g_realloc = realloc_fn;
    g_free = free_fn;
}

static unsigned char *bytes_clone(const unsigned char *src, size_t len) {
    unsigned char *dst;

    if (len == 0) {
        return NULL;
    }

    dst = g_alloc(len);
    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, len);
    return dst;
}

static ArtNode *node_create(ArtTree *tree, ArtNodeType type) {
    ArtNode *node;

    if (tree == NULL) {
        return NULL;
    }

    node = art_pool_alloc(tree_pool_for_type(tree, type));
    if (node == NULL) {
        return NULL;
    }
    node->type = type;
    return node;
}

static int node_set_prefix(ArtNode *node, const unsigned char *prefix, size_t prefix_len) {
    unsigned char *copy = NULL;
    unsigned char *old_heap = node_prefix_is_heap(node) ? node->prefix.heap : NULL;

    /* Copy before releasing the old heap prefix; callers may pass its suffix. */
    if (prefix_len > ART_INLINE_PREFIX_SIZE) {
        copy = bytes_clone(prefix, prefix_len);
        if (copy == NULL) {
            return -1;
        }
    }

    if (prefix_len > 0 && prefix_len <= ART_INLINE_PREFIX_SIZE) {
        memmove(node->prefix.inlined, prefix, prefix_len);
    }

    if (old_heap != NULL) {
        g_free(old_heap);
    }
    if (prefix_len > ART_INLINE_PREFIX_SIZE) {
        node->prefix.heap = copy;
    }
    node->prefix_len = prefix_len;
    return 0;
}

static void node_move_header(ArtNode *dst, ArtNode *src) {
    dst->count = src->count;
    dst->has_value = src->has_value;
    dst->prefix_len = src->prefix_len;
    if (node_prefix_is_heap(src)) {
        dst->prefix.heap = src->prefix.heap;
        src->prefix.heap = NULL;
    } else if (src->prefix_len > 0) {
        memcpy(dst->prefix.inlined, src->prefix.inlined, src->prefix_len);
    }
    dst->value = src->value;

    src->count = 0;
    src->has_value = 0;
    src->prefix_len = 0;
    src->value = NULL;
}

static ArtNode *node_create_leaf(ArtTree *tree,
                                 const unsigned char *prefix,
                                 size_t prefix_len,
                                 void *value) {
    ArtNode *node = node_create(tree, ART_NODE4);
    if (node == NULL) {
        return NULL;
    }
    if (node_set_prefix(node, prefix, prefix_len) != 0) {
        art_pool_free(tree_pool_for_type(tree, node->type), node);
        return NULL;
    }
    node->has_value = 1;
    node->value = value;
    return node;
}

static size_t common_prefix_len(const unsigned char *a,
                                size_t a_len,
                                const unsigned char *b,
                                size_t b_len) {
    size_t max = a_len < b_len ? a_len : b_len;
    size_t i;

    for (i = 0; i < max; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return max;
}

static int key_compare(const unsigned char *a,
                       size_t a_len,
                       const unsigned char *b,
                       size_t b_len) {
    size_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = 0;

    if (min_len > 0) {
        cmp = memcmp(a, b, min_len);
    }
    if (cmp != 0) {
        return cmp;
    }
    if (a_len == b_len) {
        return 0;
    }
    return a_len < b_len ? -1 : 1;
}

static int key_has_prefix(const unsigned char *key,
                          size_t key_len,
                          const unsigned char *prefix,
                          size_t prefix_len) {
    return key_len >= prefix_len &&
           (prefix_len == 0 || memcmp(key, prefix, prefix_len) == 0);
}

static int node_grow(ArtTree *tree, ArtNode **ref) {
    ArtNode *node = *ref;
    ArtNode *grown;
    uint16_t count = node->count;
    int i;

    if (node->type == ART_NODE4) {
        ArtNode4 old4 = *node4_data(node);

        grown = node_create(tree, ART_NODE16);
        if (grown == NULL) {
            return -1;
        }
        node_move_header(grown, node);
        for (i = 0; i < count; i++) {
            node16_data(grown)->keys[i] = old4.keys[i];
            node16_data(grown)->children[i] = old4.children[i];
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE4), node);
        *ref = grown;
        return 0;
    }

    if (node->type == ART_NODE16) {
        ArtNode16 old16 = *node16_data(node);

        grown = node_create(tree, ART_NODE48);
        if (grown == NULL) {
            return -1;
        }
        node_move_header(grown, node);
        for (i = 0; i < count; i++) {
            node48_data(grown)->child_index[old16.keys[i]] = (unsigned char)(i + 1);
            node48_data(grown)->children[i] = old16.children[i];
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE16), node);
        *ref = grown;
        return 0;
    }

    if (node->type == ART_NODE48) {
        ArtNode48 old48 = *node48_data(node);

        grown = node_create(tree, ART_NODE256);
        if (grown == NULL) {
            return -1;
        }
        node_move_header(grown, node);
        for (i = 0; i < 256; i++) {
            unsigned char idx = old48.child_index[i];
            if (idx != 0) {
                node256_data(grown)->children[i] = old48.children[idx - 1];
            }
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE48), node);
        *ref = grown;
        return 0;
    }

    return -1;
}

static void node_shrink(ArtTree *tree, ArtNode **ref) {
    ArtNode *node = *ref;
    ArtNode *shrunk;
    uint16_t count;
    int i;
    int pos;

    if (node->type == ART_NODE256 && node->count <= 48) {
        ArtNode256 old256 = *node256_data(node);

        shrunk = node_create(tree, ART_NODE48);
        if (shrunk == NULL) {
            return;
        }
        node_move_header(shrunk, node);
        pos = 0;
        for (i = 0; i < 256; i++) {
            if (old256.children[i] != NULL) {
                node48_data(shrunk)->child_index[i] = (unsigned char)(pos + 1);
                node48_data(shrunk)->children[pos++] = old256.children[i];
            }
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE256), node);
        *ref = shrunk;
        node = shrunk;
    }

    if (node->type == ART_NODE48 && node->count <= 16) {
        ArtNode48 old48 = *node48_data(node);

        shrunk = node_create(tree, ART_NODE16);
        if (shrunk == NULL) {
            return;
        }
        node_move_header(shrunk, node);
        pos = 0;
        for (i = 0; i < 256; i++) {
            unsigned char idx = old48.child_index[i];
            if (idx != 0) {
                node16_data(shrunk)->keys[pos] = (unsigned char)i;
                node16_data(shrunk)->children[pos++] = old48.children[idx - 1];
            }
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE48), node);
        *ref = shrunk;
        node = shrunk;
    }

    if (node->type == ART_NODE16 && node->count <= 4) {
        ArtNode16 old16 = *node16_data(node);

        count = node->count;
        shrunk = node_create(tree, ART_NODE4);
        if (shrunk == NULL) {
            return;
        }
        node_move_header(shrunk, node);
        for (i = 0; i < count; i++) {
            node4_data(shrunk)->keys[i] = old16.keys[i];
            node4_data(shrunk)->children[i] = old16.children[i];
        }
        art_pool_free(tree_pool_for_type(tree, ART_NODE16), node);
        *ref = shrunk;
    }
}

static ArtNode **node_child_slot(ArtNode *node, unsigned char key) {
    int i;
    unsigned char idx;

    if (node == NULL) {
        return NULL;
    }

    switch (node->type) {
        case ART_NODE4:
            for (i = 0; i < node->count; i++) {
                if (node4_data(node)->keys[i] == key) {
                    return &node4_data(node)->children[i];
                }
            }
            return NULL;
        case ART_NODE16:
            for (i = 0; i < node->count; i++) {
                if (node16_data(node)->keys[i] == key) {
                    return &node16_data(node)->children[i];
                }
            }
            return NULL;
        case ART_NODE48:
            idx = node48_data(node)->child_index[key];
            return idx == 0 ? NULL : &node48_data(node)->children[idx - 1];
        case ART_NODE256:
            return node256_data(node)->children[key] == NULL ? NULL : &node256_data(node)->children[key];
    }

    return NULL;
}

static int node_add_child(ArtTree *tree, ArtNode **ref, unsigned char key, ArtNode *child) {
    ArtNode *node = *ref;
    int pos;
    int free_pos;

    while ((node->type == ART_NODE4 && node->count == 4) ||
           (node->type == ART_NODE16 && node->count == 16) ||
           (node->type == ART_NODE48 && node->count == 48)) {
        if (node_grow(tree, ref) != 0) {
            return -1;
        }
        node = *ref;
    }

    switch (node->type) {
        case ART_NODE4:
            pos = 0;
            while (pos < node->count && node4_data(node)->keys[pos] < key) {
                pos++;
            }
            if (pos < node->count) {
                memmove(&node4_data(node)->keys[pos + 1], &node4_data(node)->keys[pos],
                        (size_t)(node->count - pos));
                memmove(&node4_data(node)->children[pos + 1], &node4_data(node)->children[pos],
                        (size_t)(node->count - pos) * sizeof(ArtNode *));
            }
            node4_data(node)->keys[pos] = key;
            node4_data(node)->children[pos] = child;
            node->count++;
            return 0;
        case ART_NODE16:
            pos = 0;
            while (pos < node->count && node16_data(node)->keys[pos] < key) {
                pos++;
            }
            if (pos < node->count) {
                memmove(&node16_data(node)->keys[pos + 1], &node16_data(node)->keys[pos],
                        (size_t)(node->count - pos));
                memmove(&node16_data(node)->children[pos + 1], &node16_data(node)->children[pos],
                        (size_t)(node->count - pos) * sizeof(ArtNode *));
            }
            node16_data(node)->keys[pos] = key;
            node16_data(node)->children[pos] = child;
            node->count++;
            return 0;
        case ART_NODE48:
            free_pos = -1;
            for (pos = 0; pos < 48; pos++) {
                if (node48_data(node)->children[pos] == NULL) {
                    free_pos = pos;
                    break;
                }
            }
            if (free_pos < 0) {
                return -1;
            }
            node48_data(node)->children[free_pos] = child;
            node48_data(node)->child_index[key] = (unsigned char)(free_pos + 1);
            node->count++;
            return 0;
        case ART_NODE256:
            if (node256_data(node)->children[key] == NULL) {
                node->count++;
            }
            node256_data(node)->children[key] = child;
            return 0;
    }

    return -1;
}

static ArtNode *node_remove_child(ArtTree *tree, ArtNode **ref, unsigned char key) {
    ArtNode *node = *ref;
    int i;
    unsigned char idx;
    ArtNode *removed;

    switch (node->type) {
        case ART_NODE4:
            for (i = 0; i < node->count; i++) {
                if (node4_data(node)->keys[i] == key) {
                    removed = node4_data(node)->children[i];
                    if (i + 1 < node->count) {
                        memmove(&node4_data(node)->keys[i], &node4_data(node)->keys[i + 1],
                                (size_t)(node->count - i - 1));
                        memmove(&node4_data(node)->children[i], &node4_data(node)->children[i + 1],
                                (size_t)(node->count - i - 1) * sizeof(ArtNode *));
                    }
                    node4_data(node)->keys[node->count - 1] = 0;
                    node4_data(node)->children[node->count - 1] = NULL;
                    node->count--;
                    return removed;
                }
            }
            return NULL;
        case ART_NODE16:
            for (i = 0; i < node->count; i++) {
                if (node16_data(node)->keys[i] == key) {
                    removed = node16_data(node)->children[i];
                    if (i + 1 < node->count) {
                        memmove(&node16_data(node)->keys[i], &node16_data(node)->keys[i + 1],
                                (size_t)(node->count - i - 1));
                        memmove(&node16_data(node)->children[i], &node16_data(node)->children[i + 1],
                                (size_t)(node->count - i - 1) * sizeof(ArtNode *));
                    }
                    node16_data(node)->keys[node->count - 1] = 0;
                    node16_data(node)->children[node->count - 1] = NULL;
                    node->count--;
                    node_shrink(tree, ref);
                    return removed;
                }
            }
            return NULL;
        case ART_NODE48:
            idx = node48_data(node)->child_index[key];
            if (idx == 0) {
                return NULL;
            }
            removed = node48_data(node)->children[idx - 1];
            node48_data(node)->children[idx - 1] = NULL;
            node48_data(node)->child_index[key] = 0;
            node->count--;
            node_shrink(tree, ref);
            return removed;
        case ART_NODE256:
            removed = node256_data(node)->children[key];
            if (removed != NULL) {
                node256_data(node)->children[key] = NULL;
                node->count--;
                node_shrink(tree, ref);
            }
            return removed;
    }

    return NULL;
}

static ArtNode *node_only_child(ArtNode *node, unsigned char *edge) {
    int i;

    if (node == NULL || node->count != 1) {
        return NULL;
    }

    switch (node->type) {
        case ART_NODE4:
            *edge = node4_data(node)->keys[0];
            return node4_data(node)->children[0];
        case ART_NODE16:
            *edge = node16_data(node)->keys[0];
            return node16_data(node)->children[0];
        case ART_NODE48:
            for (i = 0; i < 256; i++) {
                unsigned char idx = node48_data(node)->child_index[i];
                if (idx != 0) {
                    *edge = (unsigned char)i;
                    return node48_data(node)->children[idx - 1];
                }
            }
            return NULL;
        case ART_NODE256:
            for (i = 0; i < 256; i++) {
                if (node256_data(node)->children[i] != NULL) {
                    *edge = (unsigned char)i;
                    return node256_data(node)->children[i];
                }
            }
            return NULL;
    }

    return NULL;
}

static void node_free_shallow(ArtTree *tree, ArtNode *node) {
    if (node == NULL) {
        return;
    }
    node_release_prefix(node);
    art_pool_free(tree_pool_for_type(tree, node->type), node);
}

static void node_destroy(ArtTree *tree, ArtNode *node, ArtValueFreeFn free_value) {
    int i;

    if (node == NULL) {
        return;
    }

    switch (node->type) {
        case ART_NODE4:
            for (i = 0; i < node->count; i++) {
                node_destroy(tree, node4_data(node)->children[i], free_value);
            }
            break;
        case ART_NODE16:
            for (i = 0; i < node->count; i++) {
                node_destroy(tree, node16_data(node)->children[i], free_value);
            }
            break;
        case ART_NODE48:
            for (i = 0; i < 48; i++) {
                node_destroy(tree, node48_data(node)->children[i], free_value);
            }
            break;
        case ART_NODE256:
            for (i = 0; i < 256; i++) {
                node_destroy(tree, node256_data(node)->children[i], free_value);
            }
            break;
    }

    if (node->has_value && free_value != NULL) {
        free_value(node->value);
    }
    node_free_shallow(tree, node);
}

static int node_prepend_prefix(ArtNode *child, const ArtNode *parent, unsigned char edge) {
    size_t new_len = parent->prefix_len + 1 + child->prefix_len;
    unsigned char *new_prefix = g_alloc(new_len);
    size_t offset = 0;

    if (new_prefix == NULL) {
        return -1;
    }

    if (parent->prefix_len > 0) {
        memcpy(new_prefix, node_prefix_const(parent), parent->prefix_len);
        offset += parent->prefix_len;
    }
    new_prefix[offset++] = edge;
    if (child->prefix_len > 0) {
        memcpy(new_prefix + offset, node_prefix_const(child), child->prefix_len);
    }

    if (node_set_prefix(child, new_prefix, new_len) != 0) {
        g_free(new_prefix);
        return -1;
    }
    g_free(new_prefix);
    return 0;
}

static void node_cleanup_after_delete(ArtTree *tree, ArtNode **ref) {
    ArtNode *node = *ref;
    ArtNode *child;
    unsigned char edge = 0;

    if (node == NULL) {
        return;
    }

    if (node->has_value || node->count > 1) {
        return;
    }

    if (node->count == 0) {
        node_free_shallow(tree, node);
        *ref = NULL;
        return;
    }

    child = node_only_child(node, &edge);
    if (child == NULL) {
        return;
    }

    if (node_prepend_prefix(child, node, edge) != 0) {
        return;
    }

    *ref = child;
    node_free_shallow(tree, node);
}

static int insert_at(ArtTree *tree,
                     ArtNode **ref,
                     const unsigned char *key,
                     size_t key_len,
                     size_t depth,
                     void *value,
                     void **old_value) {
    ArtNode *node = *ref;
    size_t remaining_len = key_len - depth;
    size_t common;
    ArtNode *split;
    ArtNode *new_child;
    unsigned char old_edge;
    unsigned char new_edge;
    unsigned char *old_suffix;
    size_t old_suffix_len;
    ArtNode **slot;

    if (node == NULL) {
        node = node_create_leaf(tree, key + depth, remaining_len, value);
        if (node == NULL) {
            return -1;
        }
        *ref = node;
        return 1;
    }

    common = common_prefix_len(node_prefix_const(node), node->prefix_len, key + depth, remaining_len);

    if (common < node->prefix_len) {
        const unsigned char *old_prefix = node_prefix_const(node);

        split = node_create(tree, ART_NODE4);
        if (split == NULL) {
            return -1;
        }
        if (node_set_prefix(split, old_prefix, common) != 0) {
            node_free_shallow(tree, split);
            return -1;
        }

        old_edge = old_prefix[common];
        old_suffix_len = node->prefix_len - common - 1;
        old_suffix = (unsigned char *)old_prefix + common + 1;

        if (depth + common == key_len) {
            split->has_value = 1;
            split->value = value;
            new_child = NULL;
            new_edge = 0;
        } else {
            new_edge = key[depth + common];
            new_child = node_create_leaf(tree,
                                         key + depth + common + 1,
                                         key_len - depth - common - 1,
                                         value);
            if (new_child == NULL) {
                node_free_shallow(tree, split);
                return -1;
            }
        }

        if (node_set_prefix(node, old_suffix, old_suffix_len) != 0) {
            if (new_child != NULL) {
                node_destroy(tree, new_child, NULL);
            }
            node_free_shallow(tree, split);
            return -1;
        }

        if (node_add_child(tree, &split, old_edge, node) != 0) {
            if (new_child != NULL) {
                node_destroy(tree, new_child, NULL);
            }
            node_free_shallow(tree, split);
            return -1;
        }
        if (new_child != NULL && node_add_child(tree, &split, new_edge, new_child) != 0) {
            node_destroy(tree, new_child, NULL);
            node_remove_child(tree, &split, old_edge);
            node_free_shallow(tree, split);
            return -1;
        }

        *ref = split;
        return 1;
    }

    depth += node->prefix_len;
    if (depth == key_len) {
        if (node->has_value) {
            if (old_value != NULL) {
                *old_value = node->value;
            }
            node->value = value;
            return 0;
        }
        node->has_value = 1;
        node->value = value;
        return 1;
    }

    slot = node_child_slot(node, key[depth]);
    if (slot != NULL) {
        return insert_at(tree, slot, key, key_len, depth + 1, value, old_value);
    }

    new_child = node_create_leaf(tree, key + depth + 1, key_len - depth - 1, value);
    if (new_child == NULL) {
        return -1;
    }
    if (node_add_child(tree, ref, key[depth], new_child) != 0) {
        node_destroy(tree, new_child, NULL);
        return -1;
    }
    return 1;
}

static int delete_at(ArtTree *tree,
                     ArtNode **ref,
                     const unsigned char *key,
                     size_t key_len,
                     size_t depth,
                     void **old_value) {
    ArtNode *node = *ref;
    size_t remaining_len;
    ArtNode **slot;
    unsigned char edge;
    int deleted;

    if (node == NULL || key_len < depth) {
        return 0;
    }

    remaining_len = key_len - depth;
    if (remaining_len < node->prefix_len ||
        common_prefix_len(node_prefix_const(node), node->prefix_len, key + depth, remaining_len) != node->prefix_len) {
        return 0;
    }

    depth += node->prefix_len;
    if (depth == key_len) {
        if (!node->has_value) {
            return 0;
        }
        if (old_value != NULL) {
            *old_value = node->value;
        }
        node->has_value = 0;
        node->value = NULL;
        node_cleanup_after_delete(tree, ref);
        return 1;
    }

    edge = key[depth];
    slot = node_child_slot(node, edge);
    if (slot == NULL) {
        return 0;
    }

    deleted = delete_at(tree, slot, key, key_len, depth + 1, old_value);
    if (!deleted) {
        return 0;
    }

    if (*slot == NULL) {
        node_remove_child(tree, ref, edge);
    }
    node_cleanup_after_delete(tree, ref);
    return 1;
}

ArtTree *art_create(ArtValueFreeFn free_value) {
    ArtTree *tree = g_calloc(1, sizeof(*tree));
    if (tree == NULL) {
        return NULL;
    }
    tree->free_value = free_value;
    art_pool_init(&tree->pools[0], node_object_size(ART_NODE4));
    art_pool_init(&tree->pools[1], node_object_size(ART_NODE16));
    art_pool_init(&tree->pools[2], node_object_size(ART_NODE48));
    art_pool_init(&tree->pools[3], node_object_size(ART_NODE256));
    return tree;
}

void art_destroy(ArtTree *tree) {
    int i;

    if (tree == NULL) {
        return;
    }
    node_destroy(tree, tree->root, tree->free_value);
    for (i = 0; i < 4; i++) {
        art_pool_destroy(&tree->pools[i]);
    }
    g_free(tree);
}

uint64_t art_size(const ArtTree *tree) {
    return tree == NULL ? 0 : tree->size;
}

int art_insert(ArtTree *tree,
               const unsigned char *key,
               size_t key_len,
               void *value,
               void **old_value) {
    int inserted;
    void *old_local = NULL;
    void **old_slot = old_value == NULL ? &old_local : old_value;

    if (tree == NULL || (key == NULL && key_len > 0)) {
        return -1;
    }
    if (key == NULL) {
        key = g_empty_key;
    }
    if (old_value != NULL) {
        *old_value = NULL;
    }

    inserted = insert_at(tree, &tree->root, key, key_len, 0, value, old_slot);
    if (inserted > 0) {
        tree->size++;
    } else if (inserted == 0 && old_value == NULL && tree->free_value != NULL) {
        tree->free_value(old_local);
    }
    return inserted;
}

void *art_search(const ArtTree *tree, const unsigned char *key, size_t key_len) {
    ArtNode *node;
    size_t depth = 0;
    size_t remaining_len;
    ArtNode **slot;

    if (tree == NULL || (key == NULL && key_len > 0)) {
        return NULL;
    }
    if (key == NULL) {
        key = g_empty_key;
    }

    node = tree->root;
    while (node != NULL) {
        if (key_len < depth) {
            return NULL;
        }
        remaining_len = key_len - depth;
        if (remaining_len < node->prefix_len ||
            common_prefix_len(node_prefix_const(node), node->prefix_len, key + depth, remaining_len) !=
                node->prefix_len) {
            return NULL;
        }

        depth += node->prefix_len;
        if (depth == key_len) {
            return node->has_value ? node->value : NULL;
        }

        slot = node_child_slot(node, key[depth]);
        node = slot == NULL ? NULL : *slot;
        depth++;
    }

    return NULL;
}

int art_delete(ArtTree *tree,
               const unsigned char *key,
               size_t key_len,
               void **old_value) {
    int deleted;
    void *old_local = NULL;
    void **old_slot = old_value == NULL ? &old_local : old_value;

    if (tree == NULL || (key == NULL && key_len > 0)) {
        return -1;
    }
    if (key == NULL) {
        key = g_empty_key;
    }
    if (old_value != NULL) {
        *old_value = NULL;
    }

    deleted = delete_at(tree, &tree->root, key, key_len, 0, old_slot);
    if (deleted > 0) {
        tree->size--;
        if (old_value == NULL && old_local != NULL && tree->free_value != NULL) {
            tree->free_value(old_local);
        }
    }
    return deleted;
}

static int buffer_reserve(ArtKeyBuffer *buffer, size_t needed) {
    unsigned char *grown;
    size_t next_cap = buffer->cap == 0 ? 32 : buffer->cap;

    if (needed <= buffer->cap) {
        return 0;
    }

    while (next_cap < needed) {
        next_cap *= 2;
    }

    grown = g_realloc(buffer->data, next_cap);
    if (grown == NULL) {
        return -1;
    }
    buffer->data = grown;
    buffer->cap = next_cap;
    return 0;
}

static int buffer_append(ArtKeyBuffer *buffer, const unsigned char *data, size_t len) {
    if (len == 0) {
        return 0;
    }
    if (buffer_reserve(buffer, buffer->len + len) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return 0;
}

static int buffer_append_byte(ArtKeyBuffer *buffer, unsigned char byte) {
    if (buffer_reserve(buffer, buffer->len + 1) != 0) {
        return -1;
    }
    buffer->data[buffer->len++] = byte;
    return 0;
}

static void buffer_truncate(ArtKeyBuffer *buffer, size_t len) {
    buffer->len = len;
}

static int traverse_stack_push(TraverseStack *stack, const TraverseFrame *frame) {
    TraverseFrame *grown;
    size_t next_cap;

    if (stack->len == stack->cap) {
        next_cap = stack->cap == 0 ? 64 : stack->cap * 2;
        if (next_cap < stack->cap || next_cap > ((size_t)-1) / sizeof(*stack->items)) {
            return -1;
        }
        grown = g_realloc(stack->items, next_cap * sizeof(*stack->items));
        if (grown == NULL) {
            return -1;
        }
        stack->items = grown;
        stack->cap = next_cap;
    }

    stack->items[stack->len++] = *frame;
    return 0;
}

static int child_skip_empty(const ArtNode *node, int reverse, int *idx) {
    while (*idx >= 0 && *idx < 256) {
        if (node->type == ART_NODE48 && node48_data_const(node)->child_index[*idx] != 0) {
            return 1;
        }
        if (node->type == ART_NODE256 && node256_data_const(node)->children[*idx] != NULL) {
            return 1;
        }
        *idx += reverse ? -1 : 1;
    }
    return 0;
}

static int child_first(const ArtNode *node, int reverse, int *idx) {
    if (node->count == 0) {
        return 0;
    }

    switch (node->type) {
        case ART_NODE4:
        case ART_NODE16:
            *idx = reverse ? (int)node->count - 1 : 0;
            return 1;
        case ART_NODE48:
        case ART_NODE256:
            *idx = reverse ? 255 : 0;
            return child_skip_empty(node, reverse, idx);
    }

    return 0;
}

static int child_next(const ArtNode *node, int reverse, int *idx) {
    switch (node->type) {
        case ART_NODE4:
        case ART_NODE16:
            if (reverse) {
                if (*idx <= 0) {
                    return 0;
                }
                (*idx)--;
                return 1;
            }
            if (*idx + 1 >= (int)node->count) {
                return 0;
            }
            (*idx)++;
            return 1;
        case ART_NODE48:
        case ART_NODE256:
            if (reverse) {
                if (*idx <= 0) {
                    return 0;
                }
                (*idx)--;
            } else {
                if (*idx >= 255) {
                    return 0;
                }
                (*idx)++;
            }
            return child_skip_empty(node, reverse, idx);
    }

    return 0;
}

static ArtNode *child_at(const ArtNode *node, int idx, unsigned char *edge) {
    unsigned char child_idx;

    switch (node->type) {
        case ART_NODE4:
            *edge = node4_data_const(node)->keys[idx];
            return node4_data_const(node)->children[idx];
        case ART_NODE16:
            *edge = node16_data_const(node)->keys[idx];
            return node16_data_const(node)->children[idx];
        case ART_NODE48:
            child_idx = node48_data_const(node)->child_index[idx];
            if (child_idx == 0) {
                return NULL;
            }
            *edge = (unsigned char)idx;
            return node48_data_const(node)->children[child_idx - 1];
        case ART_NODE256:
            *edge = (unsigned char)idx;
            return node256_data_const(node)->children[idx];
    }

    return NULL;
}

static int path_can_match_prefix(const ArtKeyBuffer *buffer, const ArtPrefixFilter *filter) {
    size_t cmp_len;

    if (filter == NULL || filter->prefix_len == 0 || buffer->len == 0) {
        return 1;
    }

    cmp_len = buffer->len < filter->prefix_len ? buffer->len : filter->prefix_len;
    return memcmp(buffer->data, filter->prefix, cmp_len) == 0;
}

static int child_can_match_prefix(const ArtKeyBuffer *buffer,
                                  unsigned char edge,
                                  const ArtPrefixFilter *filter) {
    if (filter == NULL || buffer->len >= filter->prefix_len) {
        return 1;
    }
    return edge == filter->prefix[buffer->len];
}

static int key_has_buffer_edge_prefix(const unsigned char *key,
                                      size_t key_len,
                                      const ArtKeyBuffer *buffer,
                                      unsigned char edge) {
    if (key_len < buffer->len + 1) {
        return 0;
    }
    if (buffer->len > 0 && memcmp(key, buffer->data, buffer->len) != 0) {
        return 0;
    }
    return key[buffer->len] == edge;
}

static int buffer_edge_compare_key(const ArtKeyBuffer *buffer,
                                   unsigned char edge,
                                   const unsigned char *key,
                                   size_t key_len) {
    size_t prefix_len = buffer->len + 1;
    size_t min_len = prefix_len < key_len ? prefix_len : key_len;
    size_t buffer_cmp_len = buffer->len < min_len ? buffer->len : min_len;
    int cmp = 0;

    if (buffer_cmp_len > 0) {
        cmp = memcmp(buffer->data, key, buffer_cmp_len);
        if (cmp != 0) {
            return cmp;
        }
    }
    if (min_len > buffer->len) {
        if (edge != key[buffer->len]) {
            return edge < key[buffer->len] ? -1 : 1;
        }
    }
    if (prefix_len == key_len) {
        return 0;
    }
    return prefix_len < key_len ? -1 : 1;
}

static int path_range_state(const ArtKeyBuffer *buffer, const ArtRangeFilter *filter) {
    const unsigned char *data = buffer->len == 0 ? g_empty_key : buffer->data;
    int cmp_end;
    int cmp_start;

    if (filter == NULL) {
        return RANGE_SUBTREE_IN;
    }

    cmp_end = key_compare(data, buffer->len, filter->end, filter->end_len);
    if (cmp_end > 0) {
        return RANGE_SUBTREE_OUT;
    }
    if (cmp_end == 0) {
        return RANGE_VALUE_ONLY;
    }

    cmp_start = key_compare(data, buffer->len, filter->start, filter->start_len);
    if (cmp_start < 0 && !key_has_prefix(filter->start, filter->start_len, data, buffer->len)) {
        return RANGE_SUBTREE_OUT;
    }
    return RANGE_SUBTREE_IN;
}

static int child_range_action(const ArtKeyBuffer *buffer,
                              unsigned char edge,
                              int reverse,
                              const ArtRangeFilter *filter) {
    int cmp_end;
    int cmp_start;

    if (filter == NULL) {
        return RANGE_CHILD_VISIT;
    }

    cmp_end = buffer_edge_compare_key(buffer, edge, filter->end, filter->end_len);
    if (cmp_end > 0) {
        return reverse ? RANGE_CHILD_SKIP : RANGE_CHILD_STOP;
    }

    cmp_start = buffer_edge_compare_key(buffer, edge, filter->start, filter->start_len);
    if (cmp_start < 0 &&
        !key_has_buffer_edge_prefix(filter->start, filter->start_len, buffer, edge)) {
        return reverse ? RANGE_CHILD_STOP : RANGE_CHILD_SKIP;
    }

    return RANGE_CHILD_VISIT;
}

static int traverse_iter(const ArtTree *tree,
                         int reverse,
                         ArtIterFn callback,
                         void *ctx,
                         const ArtPrefixFilter *prefix_filter,
                         const ArtRangeFilter *range_filter) {
    ArtKeyBuffer buffer = {0};
    TraverseStack stack = {0};
    TraverseFrame root;
    int rc = 0;

    if (tree->root != NULL) {
        root.node = tree->root;
        root.base_len = 0;
        root.child_idx = TRAVERSE_ENTER;
        root.edge = 0;
        root.has_edge = 0;
        if (traverse_stack_push(&stack, &root) != 0) {
            rc = -1;
        }
    }

    while (rc == 0 && stack.len > 0) {
        TraverseFrame *frame = &stack.items[stack.len - 1];

        if (frame->child_idx == TRAVERSE_ENTER) {
            int first_idx;
            int range_state;

            if (frame->has_edge && buffer_append_byte(&buffer, frame->edge) != 0) {
                rc = -1;
                break;
            }
            if (buffer_append(&buffer, node_prefix_const(frame->node), frame->node->prefix_len) != 0) {
                rc = -1;
                break;
            }
            if (!path_can_match_prefix(&buffer, prefix_filter)) {
                buffer_truncate(&buffer, frame->base_len);
                stack.len--;
                continue;
            }
            range_state = path_range_state(&buffer, range_filter);
            if (range_state == RANGE_SUBTREE_OUT) {
                buffer_truncate(&buffer, frame->base_len);
                stack.len--;
                continue;
            }
            if (range_state == RANGE_VALUE_ONLY) {
                if (frame->node->has_value) {
                    rc = callback(buffer.data, buffer.len, frame->node->value, ctx);
                    if (rc != 0) {
                        break;
                    }
                }
                buffer_truncate(&buffer, frame->base_len);
                stack.len--;
                continue;
            }
            if (!reverse && frame->node->has_value) {
                rc = callback(buffer.data, buffer.len, frame->node->value, ctx);
                if (rc != 0) {
                    break;
                }
            }
            frame->child_idx = child_first(frame->node, reverse, &first_idx) ? first_idx : TRAVERSE_DONE;
            continue;
        }

        if (frame->child_idx == TRAVERSE_DONE) {
            if (reverse && frame->node->has_value) {
                rc = callback(buffer.data, buffer.len, frame->node->value, ctx);
                if (rc != 0) {
                    break;
                }
            }
            buffer_truncate(&buffer, frame->base_len);
            stack.len--;
            continue;
        }

        {
            TraverseFrame child_frame;
            ArtNode *child;
            unsigned char edge = 0;
            int next_idx = frame->child_idx;
            int range_action;

            child = child_at(frame->node, frame->child_idx, &edge);
            frame->child_idx = child_next(frame->node, reverse, &next_idx) ? next_idx : TRAVERSE_DONE;

            if (child == NULL || !child_can_match_prefix(&buffer, edge, prefix_filter)) {
                continue;
            }
            range_action = child_range_action(&buffer, edge, reverse, range_filter);
            if (range_action == RANGE_CHILD_STOP) {
                frame->child_idx = TRAVERSE_DONE;
                continue;
            }
            if (range_action == RANGE_CHILD_SKIP) {
                continue;
            }

            child_frame.node = child;
            child_frame.base_len = buffer.len;
            child_frame.child_idx = TRAVERSE_ENTER;
            child_frame.edge = edge;
            child_frame.has_edge = 1;
            if (traverse_stack_push(&stack, &child_frame) != 0) {
                rc = -1;
            }
        }
    }

    if (stack.items != NULL) {
        g_free(stack.items);
    }
    if (buffer.data != NULL) {
        g_free(buffer.data);
    }
    return rc;
}

int art_iter(const ArtTree *tree, ArtIterFn callback, void *ctx) {
    int rc;

    if (tree == NULL || callback == NULL) {
        return -1;
    }

    rc = traverse_iter(tree, 0, callback, ctx, NULL, NULL);
    return rc < 0 ? -1 : 0;
}

int art_reverse_iter(const ArtTree *tree, ArtIterFn callback, void *ctx) {
    int rc;

    if (tree == NULL || callback == NULL) {
        return -1;
    }

    rc = traverse_iter(tree, 1, callback, ctx, NULL, NULL);
    return rc < 0 ? -1 : 0;
}

static int range_callback(const unsigned char *key, size_t key_len, void *value, void *ctx) {
    ArtRangeCtx *range = ctx;

    if (range->emitted >= range->limit) {
        return 1;
    }
    if (key_compare(key, key_len, range->start, range->start_len) < 0 ||
        key_compare(key, key_len, range->end, range->end_len) > 0) {
        return 0;
    }

    if (range->callback(key, key_len, value, range->ctx) != 0) {
        return 1;
    }
    range->emitted++;
    return range->emitted >= range->limit ? 1 : 0;
}

int art_range(const ArtTree *tree,
              const unsigned char *start,
              size_t start_len,
              const unsigned char *end,
              size_t end_len,
              int reverse,
              size_t limit,
              ArtIterFn callback,
              void *ctx) {
    ArtRangeCtx range;
    ArtRangeFilter filter;
    int rc;

    if (tree == NULL || callback == NULL ||
        (start == NULL && start_len > 0) ||
        (end == NULL && end_len > 0)) {
        return -1;
    }
    if (start == NULL) {
        start = g_empty_key;
    }
    if (end == NULL) {
        end = g_empty_key;
    }
    if (limit == 0 || key_compare(start, start_len, end, end_len) > 0) {
        return 0;
    }

    range.start = start;
    range.start_len = start_len;
    range.end = end;
    range.end_len = end_len;
    range.limit = limit;
    range.emitted = 0;
    range.callback = callback;
    range.ctx = ctx;

    filter.start = start;
    filter.start_len = start_len;
    filter.end = end;
    filter.end_len = end_len;
    rc = traverse_iter(tree, reverse, range_callback, &range, NULL, &filter);
    return rc < 0 ? -1 : 0;
}

static int prefix_callback(const unsigned char *key, size_t key_len, void *value, void *ctx) {
    ArtPrefixCtx *prefix = ctx;

    if (prefix->emitted >= prefix->limit) {
        return 1;
    }
    if (!key_has_prefix(key, key_len, prefix->prefix, prefix->prefix_len)) {
        return 0;
    }
    if (prefix->callback(key, key_len, value, prefix->ctx) != 0) {
        return 1;
    }
    prefix->emitted++;
    return prefix->emitted >= prefix->limit ? 1 : 0;
}

int art_prefix(const ArtTree *tree,
               const unsigned char *prefix,
               size_t prefix_len,
               size_t limit,
               ArtIterFn callback,
               void *ctx) {
    ArtPrefixCtx prefix_ctx;
    ArtPrefixFilter filter;
    int rc;

    if (tree == NULL || callback == NULL || (prefix == NULL && prefix_len > 0)) {
        return -1;
    }
    if (prefix == NULL) {
        prefix = g_empty_key;
    }
    if (limit == 0) {
        return 0;
    }

    prefix_ctx.prefix = prefix;
    prefix_ctx.prefix_len = prefix_len;
    prefix_ctx.limit = limit;
    prefix_ctx.emitted = 0;
    prefix_ctx.callback = callback;
    prefix_ctx.ctx = ctx;

    filter.prefix = prefix;
    filter.prefix_len = prefix_len;
    rc = traverse_iter(tree, 0, prefix_callback, &prefix_ctx, &filter, NULL);
    return rc < 0 ? -1 : 0;
}

static uint64_t node_memory_usage(ArtNode *node, ArtValueMemUsageFn value_mem_usage) {
    uint64_t total;
    int i;

    if (node == NULL) {
        return 0;
    }

    total = node_prefix_is_heap(node) ? node->prefix_len : 0;
    if (node->has_value && value_mem_usage != NULL) {
        total += value_mem_usage(node->value);
    }

    switch (node->type) {
        case ART_NODE4:
            for (i = 0; i < node->count; i++) {
                total += node_memory_usage(node4_data(node)->children[i], value_mem_usage);
            }
            break;
        case ART_NODE16:
            for (i = 0; i < node->count; i++) {
                total += node_memory_usage(node16_data(node)->children[i], value_mem_usage);
            }
            break;
        case ART_NODE48:
            for (i = 0; i < 48; i++) {
                total += node_memory_usage(node48_data(node)->children[i], value_mem_usage);
            }
            break;
        case ART_NODE256:
            for (i = 0; i < 256; i++) {
                total += node_memory_usage(node256_data(node)->children[i], value_mem_usage);
            }
            break;
    }

    return total;
}

uint64_t art_memory_usage(const ArtTree *tree, ArtValueMemUsageFn value_mem_usage) {
    uint64_t total;
    int i;

    if (tree == NULL) {
        return 0;
    }

    total = sizeof(*tree) + node_memory_usage(tree->root, value_mem_usage);
    for (i = 0; i < 4; i++) {
        total += art_pool_memory_usage(&tree->pools[i]);
    }
    return total;
}
