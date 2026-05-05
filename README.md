# Redis Ordered Hash Module

`rtree` is a Redis Module that adds an ordered hash data type. Fields are stored in bytewise lexicographic order and can be read back with ordered iteration, inclusive ranges, prefix queries, and an incremental scan command.

## Build

```sh
make
```

The build vendors `deps/redismodule.h`, so no Redis source checkout is required.

## Load

```sh
redis-server --loadmodule ./rtree.so
```

## Commands

- `RTREE.SET key field value`: set one field, returning `1` for a new field and `0` for an update.
- `RTREE.GET key field`: return the field value, or null.
- `RTREE.DEL key field [field ...]`: delete fields and return the number removed.
- `RTREE.EXISTS key field`: return `1` if the field exists.
- `RTREE.LEN key`: return the field count.
- `RTREE.GETALL key`: return ordered `field value` pairs.
- `RTREE.KEYS key`: return ordered fields.
- `RTREE.VALS key`: return values ordered by field.
- `RTREE.RANGE key start end [LIMIT n]`: return ordered pairs whose field is in the inclusive `[start, end]` interval.
- `RTREE.REVRANGE key start end [LIMIT n]`: return the same interval in reverse order.
- `RTREE.GETPREFIX key prefix [LIMIT n]`: return ordered pairs whose field starts with `prefix`.
- `RTREE.SCAN key cursor [MATCH pattern] [COUNT n]`: incrementally scan ordered pairs.

## Examples

```redis
RTREE.SET users user:001 alice
RTREE.SET users user:010 bob
RTREE.SET users account:001 carol
RTREE.GETPREFIX users user:
RTREE.RANGE users user:000 user:999
```

## Tests

```sh
make test
```

`make test` runs the standalone ART unit test. If `redis-server` and `redis-cli` are installed, run the module integration test with:

```sh
make test-module
```

## Persistence

The module registers a Redis native data type named `rtree-art`. RDB persistence stores ordered field-value pairs and rebuilds the index on load. AOF rewrite emits `RTREE.SET` commands.
