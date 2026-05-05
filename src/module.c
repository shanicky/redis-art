#include "redismodule.h"

#include "art.h"
#include "rtree.h"
#include "rdb.h"

static RedisModuleCommandKeySpec rtree_read_key_specs[] = {
    {
        .flags = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS,
        .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
        .bs.index.pos = 1,
        .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
        .fk.range = {.lastkey = 0, .keystep = 1, .limit = 0},
    },
    {0},
};

static RedisModuleCommandKeySpec rtree_write_key_specs[] = {
    {
        .flags = REDISMODULE_CMD_KEY_RW |
                 REDISMODULE_CMD_KEY_ACCESS |
                 REDISMODULE_CMD_KEY_UPDATE,
        .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
        .bs.index.pos = 1,
        .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
        .fk.range = {.lastkey = 0, .keystep = 1, .limit = 0},
    },
    {0},
};

static RedisModuleCommandKeySpec rtree_delete_key_specs[] = {
    {
        .flags = REDISMODULE_CMD_KEY_RW |
                 REDISMODULE_CMD_KEY_ACCESS |
                 REDISMODULE_CMD_KEY_DELETE,
        .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
        .bs.index.pos = 1,
        .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
        .fk.range = {.lastkey = 0, .keystep = 1, .limit = 0},
    },
    {0},
};

static int register_command(RedisModuleCtx *ctx,
                            const char *name,
                            RedisModuleCmdFunc fn,
                            const char *flags,
                            const char *summary,
                            const char *complexity,
                            int arity,
                            RedisModuleCommandKeySpec *key_specs,
                            const char *acl_categories) {
    RedisModuleCommandInfo info = {0};
    RedisModuleCommand *command;

    if (RedisModule_CreateCommand(ctx, name, fn, flags, 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    command = RedisModule_GetCommand(ctx, name);
    if (command == NULL) {
        return REDISMODULE_ERR;
    }

    info.version = REDISMODULE_COMMAND_INFO_VERSION;
    info.summary = summary;
    info.complexity = complexity;
    info.since = "1.0.0";
    info.arity = arity;
    info.key_specs = key_specs;

    if (RedisModule_SetCommandInfo != NULL &&
        RedisModule_SetCommandInfo(command, &info) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    if (RedisModule_SetCommandACLCategories != NULL &&
        RedisModule_SetCommandACLCategories(command, acl_categories) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
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
    type_methods.free_effort = RTreeFreeEffort;
    type_methods.unlink = RTreeUnlink;
    type_methods.copy = RTreeCopy;
    type_methods.defrag = RTreeDefrag;

    RTreeType = RedisModule_CreateDataType(ctx, "rtree-art", 0, &type_methods);
    if (RTreeType == NULL) {
        return REDISMODULE_ERR;
    }

    if (register_command(ctx,
                         "rtree.set",
                         RTreeSetCommand,
                         "write deny-oom",
                         "Set a field value in an rtree key.",
                         "O(K), where K is the field length.",
                         4,
                         rtree_write_key_specs,
                         "write fast") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.get",
                         RTreeGetCommand,
                         "readonly fast",
                         "Return the value stored at a field.",
                         "O(K), where K is the field length.",
                         3,
                         rtree_read_key_specs,
                         "read fast") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.del",
                         RTreeDelCommand,
                         "write",
                         "Delete one or more fields from an rtree key.",
                         "O(N*K), where N is the number of fields and K is the average field length.",
                         -3,
                         rtree_delete_key_specs,
                         "write slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.exists",
                         RTreeExistsCommand,
                         "readonly fast",
                         "Return whether a field exists.",
                         "O(K), where K is the field length.",
                         3,
                         rtree_read_key_specs,
                         "read fast") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.len",
                         RTreeLenCommand,
                         "readonly fast",
                         "Return the number of fields in an rtree key.",
                         "O(1).",
                         2,
                         rtree_read_key_specs,
                         "read fast") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.info",
                         RTreeInfoCommand,
                         "readonly",
                         "Return metadata about an rtree key.",
                         "O(N), where N is the number of fields.",
                         2,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.getall",
                         RTreeGetAllCommand,
                         "readonly",
                         "Return all field-value pairs in field order.",
                         "O(N), where N is the number of fields.",
                         2,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.keys",
                         RTreeKeysCommand,
                         "readonly",
                         "Return all fields in field order.",
                         "O(N), where N is the number of fields.",
                         2,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.vals",
                         RTreeValsCommand,
                         "readonly",
                         "Return all values ordered by field.",
                         "O(N), where N is the number of fields.",
                         2,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.range",
                         RTreeRangeCommand,
                         "readonly",
                         "Return field-value pairs in an inclusive field range.",
                         "O(M+K), where M is the number of returned fields and K is the bound length.",
                         -4,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.revrange",
                         RTreeRevRangeCommand,
                         "readonly",
                         "Return field-value pairs in an inclusive field range in reverse order.",
                         "O(M+K), where M is the number of returned fields and K is the bound length.",
                         -4,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.getprefix",
                         RTreeGetPrefixCommand,
                         "readonly",
                         "Return field-value pairs whose field has a prefix.",
                         "O(M+K), where M is the number of returned fields and K is the prefix length.",
                         -3,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR ||
        register_command(ctx,
                         "rtree.scan",
                         RTreeScanCommand,
                         "readonly",
                         "Incrementally scan field-value pairs.",
                         "O(N) for a complete iteration.",
                         -3,
                         rtree_read_key_specs,
                         "read slow") == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
