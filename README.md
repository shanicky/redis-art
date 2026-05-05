# RTREE — Redis Ordered Hash Module

`rtree` is a Redis Module that provides an ordered hash data type backed by an Adaptive Radix Tree (ART). Fields are stored in lexicographic order and support ordered iteration, inclusive range queries, prefix matching, and incremental scan.

## Build

```sh
make
```

No Redis source checkout required — `deps/redismodule.h` is vendored.

## Load

```sh
redis-server --loadmodule ./rtree.so
```

## Commands

### CRUD

| Command | Description |
|---------|-------------|
| `RTREE.SET key field value` | Set a field. Returns `1` for new, `0` for updated. |
| `RTREE.GET key field` | Get a field's value, or null. |
| `RTREE.DEL key field [field ...]` | Delete one or more fields, returns count removed. |
| `RTREE.EXISTS key field` | Returns `1` if the field exists. |
| `RTREE.LEN key` | Returns the number of fields. |

### Traversal

| Command | Description |
|---------|-------------|
| `RTREE.GETALL key` | All field-value pairs in field order. |
| `RTREE.KEYS key` | All fields in order. |
| `RTREE.VALS key` | All values ordered by field. |

### Range & Prefix

| Command | Description |
|---------|-------------|
| `RTREE.RANGE key start end [LIMIT n]` | Ordered pairs in inclusive `[start, end]` range. |
| `RTREE.REVRANGE key start end [LIMIT n]` | Same range in reverse order. |
| `RTREE.GETPREFIX key prefix [LIMIT n]` | Ordered pairs whose field starts with `prefix`. |

### Scan

| Command | Description |
|---------|-------------|
| `RTREE.SCAN key cursor [MATCH pattern] [COUNT n]` | Incremental scan with cursor and optional glob pattern. |

## Examples

```redis
RTREE.SET users user:001 alice
RTREE.SET users user:010 bob
RTREE.SET users account:001 carol

RTREE.GETPREFIX users user:
# → user:001 alice user:010 bob

RTREE.RANGE users user:000 user:999
# → user:001 alice user:010 bob
```

## Architecture

```
src/
├── module.c       — Redis module entry point, type & command registration
├── art.c          — Adaptive Radix Tree core implementation
├── art.h          — ART public API
├── rtree.c        — RTREE data type and command implementations
├── rtree.h        — RTREE public API
├── rdb.c          — RDB save/load and AOF rewrite
└── rdb.h          — RDB public API
```

### Adaptive Radix Tree

ART compresses paths into nodes and adaptively selects node types (4, 16, 48, 256 children) based on density:

- **O(k)** lookup, insert, delete where k is the key length in bytes
- Byte-level lexicographic ordering — fields are naturally sorted
- Non-recursive stack-based traversal — safe for arbitrary key lengths
- Range queries and prefix matching with subtree pruning (skips branches that cannot match)
- Automatic node grow (4→16→48→256) and shrink (256→48→16→4) as density changes

### Memory Management

- **Slab pool allocator**: each tree maintains 4 pools (one per node type). Nodes are allocated from 64 KB slabs with an intrusive free-list for reuse. Deleting a key releases all slabs at once — O(1) teardown.
- **Prefix inlining**: compressed path prefixes ≤ 8 bytes are stored directly in the node structure, eliminating separate heap allocations for the common case.
- Memory usage is tracked through `RTREE.MEMORY key` (via `MEMORY USAGE` support).

### Persistence

The module registers a native Redis data type (`rtree-art`). RDB dumps store ordered field-value pairs and reconstructs the tree on load. AOF rewrite emits `RTREE.SET` commands.

## Tests

```sh
make test          # ART unit tests (standalone, no Redis needed)
make test-module   # Integration tests (requires redis-server & redis-cli)
```

## License

BSD 3-Clause
