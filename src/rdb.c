#include "rdb.h"

#include "rtree.h"

typedef struct {
    RedisModuleIO *io;
} SaveCtx;

typedef struct {
    RedisModuleIO *io;
    RedisModuleString *key;
} AofCtx;

static int save_entry(const unsigned char *field,
                      size_t field_len,
                      RedisModuleString *value,
                      void *ctx) {
    SaveCtx *save = ctx;

    RedisModule_SaveStringBuffer(save->io, (const char *)field, field_len);
    RedisModule_SaveString(save->io, value);
    return 0;
}

void RTreeRdbSave(RedisModuleIO *rdb, void *value) {
    RTree *hash = value;
    SaveCtx save;

    RedisModule_SaveUnsigned(rdb, RTreeLen(hash));
    save.io = rdb;
    (void)RTreeForEach(hash, 0, save_entry, &save);
}

void *RTreeRdbLoad(RedisModuleIO *rdb, int encver) {
    RTree *hash;
    uint64_t count;
    uint64_t i;

    if (encver != 0) {
        return NULL;
    }

    hash = RTreeCreate();
    if (hash == NULL) {
        return NULL;
    }

    count = RedisModule_LoadUnsigned(rdb);
    for (i = 0; i < count; i++) {
        size_t field_len = 0;
        char *field = RedisModule_LoadStringBuffer(rdb, &field_len);
        RedisModuleString *value = RedisModule_LoadString(rdb);

        if (field == NULL || value == NULL ||
            RTreeSetRaw(hash, (const unsigned char *)field, field_len, value, NULL) < 0) {
            if (field != NULL) {
                RedisModule_Free(field);
            }
            if (value != NULL) {
                RedisModule_FreeString(NULL, value);
            }
            RTreeFree(hash);
            return NULL;
        }
        RedisModule_Free(field);
    }

    return hash;
}

static int emit_aof_entry(const unsigned char *field,
                          size_t field_len,
                          RedisModuleString *value,
                          void *ctx) {
    AofCtx *aof = ctx;

    RedisModule_EmitAOF(aof->io, "RTREE.SET", "sbs", aof->key, field, field_len, value);
    return 0;
}

void RTreeAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    AofCtx ctx;

    ctx.io = aof;
    ctx.key = key;
    (void)RTreeForEach(value, 0, emit_aof_entry, &ctx);
}
