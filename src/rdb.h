#ifndef RTREE_RDB_H
#define RTREE_RDB_H

#include "redismodule.h"

void *RTreeRdbLoad(RedisModuleIO *rdb, int encver);
void RTreeRdbSave(RedisModuleIO *rdb, void *value);
void RTreeAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value);

#endif
