#include "redismodule.h"

#include "art.h"
#include "rtree.h"
#include "rdb.h"

static int register_command(RedisModuleCtx *ctx,
                            const char *name,
                            RedisModuleCmdFunc fn,
                            const char *flags) {
    return RedisModule_CreateCommand(ctx, name, fn, flags, 1, 1, 1);
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleTypeMethods type_methods = {0};

    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "rtree", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS);
    art_set_allocator(RedisModule_Alloc, RedisModule_Calloc, RedisModule_Realloc, RedisModule_Free);

    type_methods.version = REDISMODULE_TYPE_METHOD_VERSION;
    type_methods.rdb_load = RTreeRdbLoad;
    type_methods.rdb_save = RTreeRdbSave;
    type_methods.aof_rewrite = RTreeAofRewrite;
    type_methods.mem_usage = RTreeMemUsage;
    type_methods.free = RTreeFree;
    type_methods.copy = RTreeCopy;

    RTreeType = RedisModule_CreateDataType(ctx, "rtree-art", 0, &type_methods);
    if (RTreeType == NULL) {
        return REDISMODULE_ERR;
    }

    if (register_command(ctx, "rtree.set", RTreeSetCommand, "write deny-oom") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.get", RTreeGetCommand, "readonly fast") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.del", RTreeDelCommand, "write") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.exists", RTreeExistsCommand, "readonly fast") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.len", RTreeLenCommand, "readonly fast") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.getall", RTreeGetAllCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.keys", RTreeKeysCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.vals", RTreeValsCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.range", RTreeRangeCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.revrange", RTreeRevRangeCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.getprefix", RTreeGetPrefixCommand, "readonly") == REDISMODULE_ERR ||
        register_command(ctx, "rtree.scan", RTreeScanCommand, "readonly") == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
