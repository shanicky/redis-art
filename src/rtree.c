#include "rtree.h"

#include "art.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

struct RTree {
    ArtTree *index;
};

typedef struct {
    RTreeEntryCallback callback;
    void *ctx;
} EntryAdapterCtx;

typedef enum {
    REPLY_ENTRIES,
    REPLY_KEYS,
    REPLY_VALS
} ReplyMode;

typedef struct {
    RedisModuleCtx *ctx;
    ReplyMode mode;
    long long replies;
} ReplyCtx;

typedef struct {
    unsigned char *field;
    size_t field_len;
    RedisModuleString *value;
} ScanBufferEntry;

typedef struct {
    RedisModuleCtx *ctx;
    const unsigned char *pattern;
    size_t pattern_len;
    uint64_t cursor;
    uint64_t index;
    long long count;
    long long emitted;
    uint64_t next_cursor;
    long long replies;
    ScanBufferEntry *buffer;
    long long buffered;
    int failed;
    int emit;
} ScanCtx;

typedef struct {
    RTree *copy;
    int failed;
} CopyCtx;

RedisModuleType *RTreeType = NULL;

static void string_value_free(void *value) {
    if (value != NULL) {
        RedisModule_FreeString(NULL, value);
    }
}

static uint64_t string_value_mem_usage(void *value) {
    RedisModuleString *str = value;
    size_t len;

    if (str == NULL) {
        return 0;
    }
    if (RedisModule_MallocSizeString != NULL) {
        return RedisModule_MallocSizeString(str);
    }
    (void)RedisModule_StringPtrLen(str, &len);
    return (uint64_t)len;
}

RTree *RTreeCreate(void) {
    RTree *hash = RedisModule_Calloc(1, sizeof(*hash));
    if (hash == NULL) {
        return NULL;
    }

    hash->index = art_create(string_value_free);
    if (hash->index == NULL) {
        RedisModule_Free(hash);
        return NULL;
    }
    return hash;
}

void RTreeFree(void *value) {
    RTree *hash = value;

    if (hash == NULL) {
        return;
    }
    art_destroy(hash->index);
    RedisModule_Free(hash);
}

size_t RTreeMemUsage(const void *value) {
    const RTree *hash = value;

    if (hash == NULL) {
        return 0;
    }
    return sizeof(*hash) + (size_t)art_memory_usage(hash->index, string_value_mem_usage);
}

size_t RTreeFreeEffort(RedisModuleString *key, const void *value) {
    const RTree *hash = value;

    REDISMODULE_NOT_USED(key);

    if (hash == NULL) {
        return 0;
    }
    return 1 + (size_t)art_free_effort(hash->index);
}

void RTreeUnlink(RedisModuleString *key, const void *value) {
    REDISMODULE_NOT_USED(key);
    REDISMODULE_NOT_USED(value);
}

static void *rtree_defrag_alloc(void *ctx, void *ptr) {
    return RedisModule_DefragAlloc(ctx, ptr);
}

static void *rtree_defrag_value(void *ctx, void *value) {
    return RedisModule_DefragRedisModuleString(ctx, value);
}

int RTreeDefrag(RedisModuleDefragCtx *ctx, RedisModuleString *key, void **value) {
    RTree *hash;
    void *moved;

    REDISMODULE_NOT_USED(key);

    if (value == NULL || *value == NULL) {
        return 0;
    }

    hash = *value;
    moved = RedisModule_DefragAlloc(ctx, hash);
    if (moved != NULL) {
        hash = moved;
        *value = hash;
    }

    hash->index = art_defrag(hash->index, rtree_defrag_alloc, rtree_defrag_value, ctx);
    return 0;
}

uint64_t RTreeLen(const RTree *hash) {
    return hash == NULL ? 0 : art_size(hash->index);
}

RedisModuleString *RTreeGet(const RTree *hash, const unsigned char *field, size_t field_len) {
    if (hash == NULL) {
        return NULL;
    }
    return art_search(hash->index, field, field_len);
}

int RTreeSetRaw(RTree *hash,
                const unsigned char *field,
                size_t field_len,
                RedisModuleString *value,
                RedisModuleString **old_value) {
    void *old_raw = NULL;
    int rc;

    if (hash == NULL || value == NULL) {
        return -1;
    }
    rc = art_insert(hash->index, field, field_len, value, &old_raw);
    if (old_value != NULL) {
        *old_value = old_raw;
    }
    return rc;
}

int RTreeDeleteRaw(RTree *hash,
                   const unsigned char *field,
                   size_t field_len,
                   RedisModuleString **old_value) {
    void *old_raw = NULL;
    int rc;

    if (hash == NULL) {
        return 0;
    }
    rc = art_delete(hash->index, field, field_len, &old_raw);
    if (old_value != NULL) {
        *old_value = old_raw;
    }
    return rc;
}

static int entry_adapter(const unsigned char *field, size_t field_len, void *value, void *ctx) {
    EntryAdapterCtx *adapter = ctx;
    return adapter->callback(field, field_len, value, adapter->ctx);
}

int RTreeForEach(const RTree *hash, int reverse, RTreeEntryCallback callback, void *ctx) {
    EntryAdapterCtx adapter;

    if (hash == NULL || callback == NULL) {
        return -1;
    }

    adapter.callback = callback;
    adapter.ctx = ctx;
    return reverse ? art_reverse_iter(hash->index, entry_adapter, &adapter)
                   : art_iter(hash->index, entry_adapter, &adapter);
}

int RTreeRange(const RTree *hash,
               const unsigned char *start,
               size_t start_len,
               const unsigned char *end,
               size_t end_len,
               int reverse,
               size_t limit,
               RTreeEntryCallback callback,
               void *ctx) {
    EntryAdapterCtx adapter;

    if (hash == NULL || callback == NULL) {
        return -1;
    }

    adapter.callback = callback;
    adapter.ctx = ctx;
    return art_range(hash->index, start, start_len, end, end_len, reverse, limit, entry_adapter, &adapter);
}

int RTreePrefix(const RTree *hash,
                const unsigned char *prefix,
                size_t prefix_len,
                size_t limit,
                RTreeEntryCallback callback,
                void *ctx) {
    EntryAdapterCtx adapter;

    if (hash == NULL || callback == NULL) {
        return -1;
    }

    adapter.callback = callback;
    adapter.ctx = ctx;
    return art_prefix(hash->index, prefix, prefix_len, limit, entry_adapter, &adapter);
}

static int copy_entry(const unsigned char *field,
                      size_t field_len,
                      RedisModuleString *value,
                      void *ctx) {
    CopyCtx *copy_ctx = ctx;
    RedisModuleString *value_copy;

    value_copy = RedisModule_CreateStringFromString(NULL, value);
    if (value_copy == NULL || RTreeSetRaw(copy_ctx->copy, field, field_len, value_copy, NULL) < 0) {
        if (value_copy != NULL) {
            RedisModule_FreeString(NULL, value_copy);
        }
        copy_ctx->failed = 1;
        return 1;
    }
    return 0;
}

void *RTreeCopy(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value) {
    const RTree *source = value;
    CopyCtx copy_ctx;

    REDISMODULE_NOT_USED(fromkey);
    REDISMODULE_NOT_USED(tokey);

    copy_ctx.copy = RTreeCreate();
    copy_ctx.failed = 0;
    if (copy_ctx.copy == NULL) {
        return NULL;
    }

    if (RTreeForEach(source, 0, copy_entry, &copy_ctx) != 0 || copy_ctx.failed) {
        RTreeFree(copy_ctx.copy);
        return NULL;
    }
    return copy_ctx.copy;
}

static int rmstr_eq_ci(RedisModuleString *str, const char *literal) {
    size_t len;
    const char *ptr = RedisModule_StringPtrLen(str, &len);
    size_t literal_len = strlen(literal);
    size_t i;

    if (len != literal_len) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (tolower((unsigned char)ptr[i]) != tolower((unsigned char)literal[i])) {
            return 0;
        }
    }
    return 1;
}

static int parse_nonnegative_ll(RedisModuleCtx *ctx,
                                RedisModuleString *arg,
                                const char *name,
                                long long *out) {
    long long value;
    char error[96];

    if (RedisModule_StringToLongLong(arg, &value) != REDISMODULE_OK || value < 0) {
        snprintf(error, sizeof(error), "ERR %s must be a non-negative integer", name);
        RedisModule_ReplyWithError(ctx, error);
        return REDISMODULE_ERR;
    }
    *out = value;
    return REDISMODULE_OK;
}

static int parse_positive_ll(RedisModuleCtx *ctx,
                             RedisModuleString *arg,
                             const char *name,
                             long long *out) {
    long long value;
    char error[96];

    if (RedisModule_StringToLongLong(arg, &value) != REDISMODULE_OK || value <= 0) {
        snprintf(error, sizeof(error), "ERR %s must be a positive integer", name);
        RedisModule_ReplyWithError(ctx, error);
        return REDISMODULE_ERR;
    }
    *out = value;
    return REDISMODULE_OK;
}

static int parse_limit(RedisModuleCtx *ctx,
                       RedisModuleString **argv,
                       int argc,
                       int pos,
                       size_t *limit) {
    long long parsed;

    *limit = ART_LIMIT_UNLIMITED;
    if (argc == pos) {
        return REDISMODULE_OK;
    }
    if (argc != pos + 2 || !rmstr_eq_ci(argv[pos], "LIMIT")) {
        RedisModule_ReplyWithError(ctx, "ERR syntax error");
        return REDISMODULE_ERR;
    }
    if (parse_nonnegative_ll(ctx, argv[pos + 1], "LIMIT", &parsed) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }
    *limit = (size_t)parsed;
    return REDISMODULE_OK;
}

static int open_rtree_key(RedisModuleCtx *ctx,
                          RedisModuleString *keyname,
                          int mode,
                          int create,
                          RedisModuleKey **key,
                          RTree **hash) {
    int key_type;

    *key = RedisModule_OpenKey(ctx, keyname, mode);
    *hash = NULL;
    key_type = RedisModule_KeyType(*key);

    if (key_type == REDISMODULE_KEYTYPE_EMPTY) {
        if (!create) {
            return REDISMODULE_OK;
        }
        *hash = RTreeCreate();
        if (*hash == NULL) {
            RedisModule_CloseKey(*key);
            *key = NULL;
            RedisModule_ReplyWithError(ctx, "OOM could not allocate rtree");
            return REDISMODULE_ERR;
        }
        if (RedisModule_ModuleTypeSetValue(*key, RTreeType, *hash) != REDISMODULE_OK) {
            RTreeFree(*hash);
            RedisModule_CloseKey(*key);
            *key = NULL;
            *hash = NULL;
            RedisModule_ReplyWithError(ctx, "ERR could not initialize rtree key");
            return REDISMODULE_ERR;
        }
        return REDISMODULE_OK;
    }

    if (key_type != REDISMODULE_KEYTYPE_MODULE || RedisModule_ModuleTypeGetType(*key) != RTreeType) {
        RedisModule_CloseKey(*key);
        *key = NULL;
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }

    *hash = RedisModule_ModuleTypeGetValue(*key);
    return REDISMODULE_OK;
}

static void close_key_if_open(RedisModuleKey *key) {
    if (key != NULL) {
        RedisModule_CloseKey(key);
    }
}

static void notify_rtree_event(RedisModuleCtx *ctx, const char *event, RedisModuleString *keyname) {
    (void)RedisModule_NotifyKeyspaceEvent(ctx, REDISMODULE_NOTIFY_MODULE, event, keyname);
}

int RTreeSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    RedisModuleString *new_value;
    RedisModuleString *old_value = NULL;
    const char *field;
    size_t field_len;
    int rc;

    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    if (open_rtree_key(ctx, argv[1], REDISMODULE_WRITE, 1, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }

    field = RedisModule_StringPtrLen(argv[2], &field_len);
    new_value = RedisModule_CreateStringFromString(ctx, argv[3]);
    if (new_value == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithError(ctx, "OOM could not allocate value");
        return REDISMODULE_OK;
    }

    rc = RTreeSetRaw(hash, (const unsigned char *)field, field_len, new_value, &old_value);
    if (rc < 0) {
        RedisModule_FreeString(ctx, new_value);
        close_key_if_open(key);
        RedisModule_ReplyWithError(ctx, "OOM could not update rtree");
        return REDISMODULE_OK;
    }

    if (old_value != NULL) {
        RedisModule_FreeString(ctx, old_value);
    }
    RedisModule_ReplicateVerbatim(ctx);
    notify_rtree_event(ctx, "rtree.set", argv[1]);
    close_key_if_open(key);
    RedisModule_ReplyWithLongLong(ctx, rc > 0 ? 1 : 0);
    return REDISMODULE_OK;
}

int RTreeGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    RedisModuleString *value;
    const char *field;
    size_t field_len;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (hash == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }

    field = RedisModule_StringPtrLen(argv[2], &field_len);
    value = RTreeGet(hash, (const unsigned char *)field, field_len);
    close_key_if_open(key);
    if (value == NULL) {
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithString(ctx, value);
    }
    return REDISMODULE_OK;
}

int RTreeDelCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    long long deleted = 0;
    int i;

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_WRITE, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (hash == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithLongLong(ctx, 0);
        return REDISMODULE_OK;
    }

    for (i = 2; i < argc; i++) {
        const char *field;
        size_t field_len;
        RedisModuleString *old_value = NULL;

        field = RedisModule_StringPtrLen(argv[i], &field_len);
        if (RTreeDeleteRaw(hash, (const unsigned char *)field, field_len, &old_value) > 0) {
            deleted++;
            RedisModule_FreeString(ctx, old_value);
        }
    }

    if (deleted > 0) {
        int emptied = RTreeLen(hash) == 0;

        if (emptied) {
            RedisModule_DeleteKey(key);
        }
        RedisModule_ReplicateVerbatim(ctx);
        notify_rtree_event(ctx, "rtree.del", argv[1]);
        if (emptied) {
            notify_rtree_event(ctx, "rtree.empty", argv[1]);
        }
    }
    close_key_if_open(key);
    RedisModule_ReplyWithLongLong(ctx, deleted);
    return REDISMODULE_OK;
}

int RTreeExistsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    const char *field;
    size_t field_len;
    int exists;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (hash == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithLongLong(ctx, 0);
        return REDISMODULE_OK;
    }
    field = RedisModule_StringPtrLen(argv[2], &field_len);
    exists = RTreeGet(hash, (const unsigned char *)field, field_len) != NULL;
    close_key_if_open(key);
    RedisModule_ReplyWithLongLong(ctx, exists ? 1 : 0);
    return REDISMODULE_OK;
}

int RTreeLenCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    uint64_t len;

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    len = RTreeLen(hash);
    close_key_if_open(key);
    RedisModule_ReplyWithLongLong(ctx, (long long)len);
    return REDISMODULE_OK;
}

int RTreeInfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    uint64_t len;
    size_t memory_usage;

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }

    len = RTreeLen(hash);
    memory_usage = hash == NULL ? 0 : RTreeMemUsage(hash);
    close_key_if_open(key);

    RedisModule_ReplyWithMap(ctx, 4);
    RedisModule_ReplyWithCString(ctx, "type");
    RedisModule_ReplyWithCString(ctx, "rtree-art");
    RedisModule_ReplyWithCString(ctx, "encoding");
    RedisModule_ReplyWithCString(ctx, "art");
    RedisModule_ReplyWithCString(ctx, "length");
    RedisModule_ReplyWithLongLong(ctx, (long long)len);
    RedisModule_ReplyWithCString(ctx, "memory_usage");
    RedisModule_ReplyWithLongLong(ctx, (long long)memory_usage);
    return REDISMODULE_OK;
}

static int reply_entry(const unsigned char *field,
                       size_t field_len,
                       RedisModuleString *value,
                       void *ctx) {
    ReplyCtx *reply = ctx;

    if (reply->mode == REPLY_ENTRIES || reply->mode == REPLY_KEYS) {
        RedisModule_ReplyWithStringBuffer(reply->ctx, (const char *)field, field_len);
        reply->replies++;
    }
    if (reply->mode == REPLY_ENTRIES || reply->mode == REPLY_VALS) {
        RedisModule_ReplyWithString(reply->ctx, value);
        reply->replies++;
    }
    return 0;
}

static int reply_collection(RedisModuleCtx *ctx, RTree *hash, ReplyMode mode) {
    ReplyCtx reply;

    if (hash == NULL) {
        RedisModule_ReplyWithArray(ctx, 0);
        return REDISMODULE_OK;
    }

    reply.ctx = ctx;
    reply.mode = mode;
    reply.replies = 0;
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
    (void)RTreeForEach(hash, 0, reply_entry, &reply);
    RedisModule_ReplySetArrayLength(ctx, reply.replies);
    return REDISMODULE_OK;
}

int RTreeGetAllCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    reply_collection(ctx, hash, REPLY_ENTRIES);
    close_key_if_open(key);
    return REDISMODULE_OK;
}

int RTreeKeysCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    reply_collection(ctx, hash, REPLY_KEYS);
    close_key_if_open(key);
    return REDISMODULE_OK;
}

int RTreeValsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;

    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    reply_collection(ctx, hash, REPLY_VALS);
    close_key_if_open(key);
    return REDISMODULE_OK;
}

static int range_common_command(RedisModuleCtx *ctx,
                                RedisModuleString **argv,
                                int argc,
                                int reverse) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    const char *start;
    const char *end;
    size_t start_len;
    size_t end_len;
    size_t limit;
    ReplyCtx reply;

    if (argc != 4 && argc != 6) {
        return RedisModule_WrongArity(ctx);
    }
    if (parse_limit(ctx, argv, argc, 4, &limit) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (hash == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithArray(ctx, 0);
        return REDISMODULE_OK;
    }

    start = RedisModule_StringPtrLen(argv[2], &start_len);
    end = RedisModule_StringPtrLen(argv[3], &end_len);
    reply.ctx = ctx;
    reply.mode = REPLY_ENTRIES;
    reply.replies = 0;
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
    (void)RTreeRange(hash,
                     (const unsigned char *)start,
                     start_len,
                     (const unsigned char *)end,
                     end_len,
                     reverse,
                     limit,
                     reply_entry,
                     &reply);
    RedisModule_ReplySetArrayLength(ctx, reply.replies);
    close_key_if_open(key);
    return REDISMODULE_OK;
}

int RTreeRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return range_common_command(ctx, argv, argc, 0);
}

int RTreeRevRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return range_common_command(ctx, argv, argc, 1);
}

int RTreeGetPrefixCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    const char *prefix;
    size_t prefix_len;
    size_t limit;
    ReplyCtx reply;

    if (argc != 3 && argc != 5) {
        return RedisModule_WrongArity(ctx);
    }
    if (parse_limit(ctx, argv, argc, 3, &limit) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }
    if (hash == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithArray(ctx, 0);
        return REDISMODULE_OK;
    }

    prefix = RedisModule_StringPtrLen(argv[2], &prefix_len);
    reply.ctx = ctx;
    reply.mode = REPLY_ENTRIES;
    reply.replies = 0;
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
    (void)RTreePrefix(hash, (const unsigned char *)prefix, prefix_len, limit, reply_entry, &reply);
    RedisModule_ReplySetArrayLength(ctx, reply.replies);
    close_key_if_open(key);
    return REDISMODULE_OK;
}

static int glob_match_class(const unsigned char *pattern,
                            size_t pattern_len,
                            size_t *pos,
                            unsigned char ch) {
    size_t i = *pos + 1;
    int negate = 0;
    int matched = 0;
    int closed = 0;

    if (i < pattern_len && (pattern[i] == '^' || pattern[i] == '!')) {
        negate = 1;
        i++;
    }

    while (i < pattern_len) {
        unsigned char start = pattern[i];
        unsigned char end = start;

        if (start == ']' && i > *pos + 1 + (size_t)negate) {
            closed = 1;
            i++;
            break;
        }
        if (start == '\\' && i + 1 < pattern_len) {
            start = pattern[++i];
        }
        if (i + 2 < pattern_len && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
            i += 2;
            end = pattern[i];
            if (end == '\\' && i + 1 < pattern_len) {
                end = pattern[++i];
            }
        }
        if (start <= ch && ch <= end) {
            matched = 1;
        }
        i++;
    }

    if (!closed) {
        return ch == '[';
    }

    *pos = i - 1;
    return negate ? !matched : matched;
}

static int glob_match_at(const unsigned char *pattern,
                         size_t pattern_len,
                         size_t ppos,
                         const unsigned char *str,
                         size_t str_len,
                         size_t spos) {
    while (ppos < pattern_len) {
        unsigned char token = pattern[ppos];

        if (token == '*') {
            while (ppos + 1 < pattern_len && pattern[ppos + 1] == '*') {
                ppos++;
            }
            if (ppos + 1 == pattern_len) {
                return 1;
            }
            while (spos <= str_len) {
                if (glob_match_at(pattern, pattern_len, ppos + 1, str, str_len, spos)) {
                    return 1;
                }
                if (spos == str_len) {
                    break;
                }
                spos++;
            }
            return 0;
        }

        if (spos == str_len) {
            return 0;
        }

        if (token == '?') {
            ppos++;
            spos++;
            continue;
        }

        if (token == '[') {
            if (!glob_match_class(pattern, pattern_len, &ppos, str[spos])) {
                return 0;
            }
            ppos++;
            spos++;
            continue;
        }

        if (token == '\\' && ppos + 1 < pattern_len) {
            token = pattern[++ppos];
        }
        if (token != str[spos]) {
            return 0;
        }
        ppos++;
        spos++;
    }

    return spos == str_len;
}

static int glob_match(const unsigned char *pattern,
                      size_t pattern_len,
                      const unsigned char *str,
                      size_t str_len) {
    return glob_match_at(pattern, pattern_len, 0, str, str_len, 0);
}

static void scan_buffer_clear(ScanCtx *scan) {
    long long i;

    if (scan->buffer == NULL) {
        return;
    }
    for (i = 0; i < scan->buffered; i++) {
        if (scan->buffer[i].field != NULL) {
            RedisModule_Free(scan->buffer[i].field);
        }
    }
    RedisModule_Free(scan->buffer);
    scan->buffer = NULL;
    scan->buffered = 0;
}

static int scan_buffer_append(ScanCtx *scan,
                              const unsigned char *field,
                              size_t field_len,
                              RedisModuleString *value) {
    ScanBufferEntry *entry = &scan->buffer[scan->buffered];

    entry->field = NULL;
    entry->field_len = field_len;
    entry->value = value;
    if (field_len > 0) {
        entry->field = RedisModule_Alloc(field_len);
        if (entry->field == NULL) {
            scan->failed = 1;
            return -1;
        }
        memcpy(entry->field, field, field_len);
    }
    scan->buffered++;
    return 0;
}

static int scan_entry(const unsigned char *field,
                      size_t field_len,
                      RedisModuleString *value,
                      void *ctx) {
    ScanCtx *scan = ctx;
    uint64_t current = scan->index++;

    if (current < scan->cursor) {
        return 0;
    }

    if (scan->pattern != NULL && !glob_match(scan->pattern, scan->pattern_len, field, field_len)) {
        return 0;
    }

    if (scan->buffer != NULL) {
        if (scan_buffer_append(scan, field, field_len, value) != 0) {
            return -1;
        }
        scan->emitted++;
        if (scan->buffered >= scan->count) {
            scan->next_cursor = current + 1;
            return 1;
        }
        return 0;
    }

    if (scan->emit) {
        RedisModule_ReplyWithStringBuffer(scan->ctx, (const char *)field, field_len);
        RedisModule_ReplyWithString(scan->ctx, value);
        scan->replies += 2;
    }
    scan->emitted++;

    if (scan->emitted >= scan->count) {
        scan->next_cursor = current + 1;
        return 1;
    }
    return 0;
}

int RTreeScanCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key = NULL;
    RTree *hash = NULL;
    long long cursor_ll;
    long long count = 10;
    uint64_t len;
    const unsigned char *pattern = NULL;
    size_t pattern_len = 0;
    ScanCtx scan;
    char cursor_buf[32];
    int pos = 3;

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }
    if (parse_nonnegative_ll(ctx, argv[2], "cursor", &cursor_ll) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }

    while (pos < argc) {
        if (rmstr_eq_ci(argv[pos], "MATCH")) {
            if (pos + 1 >= argc || pattern != NULL) {
                RedisModule_ReplyWithError(ctx, "ERR syntax error");
                return REDISMODULE_OK;
            }
            pattern = (const unsigned char *)RedisModule_StringPtrLen(argv[pos + 1], &pattern_len);
            pos += 2;
        } else if (rmstr_eq_ci(argv[pos], "COUNT")) {
            if (pos + 1 >= argc) {
                RedisModule_ReplyWithError(ctx, "ERR syntax error");
                return REDISMODULE_OK;
            }
            if (parse_positive_ll(ctx, argv[pos + 1], "COUNT", &count) != REDISMODULE_OK) {
                return REDISMODULE_OK;
            }
            pos += 2;
        } else {
            RedisModule_ReplyWithError(ctx, "ERR syntax error");
            return REDISMODULE_OK;
        }
    }

    if (open_rtree_key(ctx, argv[1], REDISMODULE_READ, 0, &key, &hash) != REDISMODULE_OK) {
        return REDISMODULE_OK;
    }

    len = RTreeLen(hash);
    if (hash == NULL || (uint64_t)cursor_ll >= len) {
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithCString(ctx, "0");
        RedisModule_ReplyWithArray(ctx, 0);
        close_key_if_open(key);
        return REDISMODULE_OK;
    }

    scan.ctx = ctx;
    scan.pattern = pattern;
    scan.pattern_len = pattern_len;
    scan.cursor = (uint64_t)cursor_ll;
    scan.index = 0;
    scan.count = count;
    scan.emitted = 0;
    scan.next_cursor = 0;
    scan.replies = 0;
    scan.buffer = NULL;
    scan.buffered = 0;
    scan.failed = 0;
    scan.emit = 0;

    if (pattern == NULL) {
        if ((uint64_t)count >= len - scan.cursor) {
            scan.next_cursor = 0;
        } else {
            scan.next_cursor = scan.cursor + (uint64_t)count;
        }
        snprintf(cursor_buf, sizeof(cursor_buf), "%llu", (unsigned long long)scan.next_cursor);
        RedisModule_ReplyWithArray(ctx, 2);
        RedisModule_ReplyWithCString(ctx, cursor_buf);

        scan.emit = 1;
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_LEN);
        (void)RTreeForEach(hash, 0, scan_entry, &scan);
        RedisModule_ReplySetArrayLength(ctx, scan.replies);

        close_key_if_open(key);
        return REDISMODULE_OK;
    }

    if ((unsigned long long)count > ((unsigned long long)((size_t)-1) / sizeof(*scan.buffer))) {
        close_key_if_open(key);
        RedisModule_ReplyWithError(ctx, "ERR COUNT is too large");
        return REDISMODULE_OK;
    }
    scan.buffer = RedisModule_Calloc((size_t)count, sizeof(*scan.buffer));
    if (scan.buffer == NULL) {
        close_key_if_open(key);
        RedisModule_ReplyWithError(ctx, "OOM could not allocate scan buffer");
        return REDISMODULE_OK;
    }

    if (RTreeForEach(hash, 0, scan_entry, &scan) < 0 || scan.failed) {
        scan_buffer_clear(&scan);
        close_key_if_open(key);
        RedisModule_ReplyWithError(ctx, "OOM could not buffer scan result");
        return REDISMODULE_OK;
    }
    if (scan.next_cursor >= len) {
        scan.next_cursor = 0;
    }
    snprintf(cursor_buf, sizeof(cursor_buf), "%llu", (unsigned long long)scan.next_cursor);
    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithCString(ctx, cursor_buf);

    RedisModule_ReplyWithArray(ctx, scan.buffered * 2);
    for (scan.index = 0; scan.index < (uint64_t)scan.buffered; scan.index++) {
        ScanBufferEntry *entry = &scan.buffer[scan.index];
        const char *field = entry->field_len == 0 ? "" : (const char *)entry->field;
        RedisModule_ReplyWithStringBuffer(scan.ctx, field, entry->field_len);
        RedisModule_ReplyWithString(scan.ctx, entry->value);
    }
    scan_buffer_clear(&scan);

    close_key_if_open(key);
    return REDISMODULE_OK;
}
