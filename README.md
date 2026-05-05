# Redis Ordered Hash Module

`rtree` is a Redis module that adds an ordered hash data type. It stores
field-value pairs in an adaptive radix tree (ART), so fields are kept in
bytewise lexicographic order and can be read through ordered iteration,
inclusive ranges, prefix queries, and cursor-based scans.

The module registers itself as `rtree` and creates a native Redis data type
named `rtree-art`.

## Build

```sh
make
```

The build uses the vendored `deps/redismodule.h`, so a Redis source checkout is
not required. The default build output is `rtree.so`.

To remove local build artifacts:

```sh
make clean
```

## Load

Start Redis with the module:

```sh
redis-server --loadmodule ./rtree.so
```

Then use the commands with `redis-cli`:

```sh
redis-cli RTREE.SET users user:001 alice
redis-cli RTREE.GET users user:001
```

## Command Reference

All field ordering is bytewise lexicographic ordering over the raw field bytes.
Range bounds are inclusive.

### Writes

- `RTREE.SET key field value`
  - Sets `field` to `value`.
  - Returns `1` when a new field is inserted.
  - Returns `0` when an existing field is updated.

- `RTREE.DEL key field [field ...]`
  - Deletes one or more fields.
  - Returns the number of fields removed.
  - Deletes the Redis key when the last field is removed.

### Reads

- `RTREE.GET key field`
  - Returns the field value.
  - Returns null when the key or field does not exist.

- `RTREE.EXISTS key field`
  - Returns `1` when the field exists.
  - Returns `0` otherwise.

- `RTREE.LEN key`
  - Returns the number of fields stored at `key`.
  - Returns `0` for a missing key.

- `RTREE.GETALL key`
  - Returns ordered `field value` pairs.

- `RTREE.KEYS key`
  - Returns ordered fields.

- `RTREE.VALS key`
  - Returns values ordered by field.

### Ordered Queries

- `RTREE.RANGE key start end [LIMIT n]`
  - Returns ordered `field value` pairs whose field is in the inclusive
    `[start, end]` interval.
  - `LIMIT n` is optional and must be a non-negative integer.
  - `LIMIT 0` returns no pairs.

- `RTREE.REVRANGE key start end [LIMIT n]`
  - Returns the same inclusive interval in reverse field order.
  - `LIMIT n` has the same behavior as `RTREE.RANGE`.

- `RTREE.GETPREFIX key prefix [LIMIT n]`
  - Returns ordered `field value` pairs whose field starts with `prefix`.
  - `LIMIT n` is optional and must be a non-negative integer.

- `RTREE.SCAN key cursor [MATCH pattern] [COUNT n]`
  - Returns `[next-cursor, [field, value, ...]]`.
  - Starts scanning from `cursor`, where `0` starts a new scan.
  - Returns next cursor `0` when the scan is complete.
  - `MATCH pattern` is optional and supports Redis-style glob matching for
    `*`, `?`, character classes, negated classes, ranges, and backslash escapes.
  - `COUNT n` is optional, defaults to `10`, and must be a positive integer.

## Examples

```redis
RTREE.SET users user:001 alice
RTREE.SET users user:010 bob
RTREE.SET users account:001 carol

RTREE.GET users user:001
RTREE.KEYS users
RTREE.GETPREFIX users user:
RTREE.RANGE users user:000 user:999
RTREE.REVRANGE users account:000 user:999 LIMIT 2
RTREE.SCAN users 0 MATCH user:* COUNT 10
```

## Development

Main source files:

- `src/module.c`: Redis module initialization, data type registration, and
  command registration.
- `src/rtree.c`: Redis command implementations and `RTree` wrapper logic.
- `src/art.c`: Adaptive radix tree implementation used for ordered storage.
- `src/rdb.c`: RDB load/save and AOF rewrite support.
- `tests/test_art.c`: Standalone ART unit tests.
- `tests/test_module.rb`: Redis module integration test driven through
  `redis-server` and `redis-cli`.

The project is C99 and is built by `make` with `-Wall`, `-Wextra`, and
`-Wpedantic`.

## Tests

Run the standalone ART unit test:

```sh
make test
```

`make test` builds and runs `tests/test_art`. It does not require Redis.

Run the Redis module integration test:

```sh
make test-module
```

`make test-module` requires `redis-server`, `redis-cli`, Ruby, and a built
`rtree.so`. The test starts a temporary Redis server, loads the module, verifies
command behavior, and shuts the server down.

## Continuous Integration

GitHub Actions runs the workflow in `.github/workflows/ci.yml` for pushes to
`master`, pull requests, and manual dispatches. The CI job installs the Redis
tools and Ruby on Ubuntu, builds the module, runs the standalone ART unit test,
and runs the Redis module integration test.

## Persistence

RDB persistence stores the ordered field-value pairs and rebuilds the ART index
on load. AOF rewrite emits `RTREE.SET` commands for each stored pair.

## Build Artifacts

The following generated files are intentionally ignored by Git:

- `rtree.so`
- `src/*.o`
- `tests/test_art`
