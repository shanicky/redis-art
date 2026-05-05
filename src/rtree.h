#ifndef RTREE_H
#define RTREE_H

#include "redismodule.h"

#include <stddef.h>
#include <stdint.h>

typedef struct RTree RTree;

typedef int (*RTreeEntryCallback)(const unsigned char *field,
                                  size_t field_len,
                                  RedisModuleString *value,
                                  void *ctx);

extern RedisModuleType *RTreeType;

RTree *RTreeCreate(void);
void RTreeFree(void *value);
size_t RTreeMemUsage(const void *value);
size_t RTreeFreeEffort(RedisModuleString *key, const void *value);
void RTreeUnlink(RedisModuleString *key, const void *value);
int RTreeDefrag(RedisModuleDefragCtx *ctx, RedisModuleString *key, void **value);
void *RTreeCopy(RedisModuleString *fromkey, RedisModuleString *tokey, const void *value);

uint64_t RTreeLen(const RTree *hash);
RedisModuleString *RTreeGet(const RTree *hash, const unsigned char *field, size_t field_len);
int RTreeSetRaw(RTree *hash,
                const unsigned char *field,
                size_t field_len,
                RedisModuleString *value,
                RedisModuleString **old_value);
int RTreeDeleteRaw(RTree *hash,
                   const unsigned char *field,
                   size_t field_len,
                   RedisModuleString **old_value);

int RTreeForEach(const RTree *hash, int reverse, RTreeEntryCallback callback, void *ctx);
int RTreeRange(const RTree *hash,
               const unsigned char *start,
               size_t start_len,
               const unsigned char *end,
               size_t end_len,
               int reverse,
               size_t limit,
               RTreeEntryCallback callback,
               void *ctx);
int RTreePrefix(const RTree *hash,
                const unsigned char *prefix,
                size_t prefix_len,
                size_t limit,
                RTreeEntryCallback callback,
                void *ctx);

int RTreeSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeGetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeDelCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeExistsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeLenCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeInfoCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeGetAllCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeKeysCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeValsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeRevRangeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeGetPrefixCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RTreeScanCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);

#endif
