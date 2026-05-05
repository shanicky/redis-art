# AI Agent Instructions

This file is the source of truth for AI assistants working in this repository.
Keep assistant-specific files short and point them back here when possible.

## Project Overview

`rtree` is a Redis module written in C99. It implements an ordered field-value
map backed by an adaptive radix tree (ART). Fields are ordered by raw bytewise
lexicographic order.

Core files:

- `src/module.c`: module initialization, Redis data type registration, and
  command registration.
- `src/rtree.c`: Redis command handlers, option parsing, scan glob matching,
  and the `RTree` wrapper around the ART.
- `src/art.c`: adaptive radix tree implementation and ordered iteration.
- `src/rdb.c`: RDB save/load and AOF rewrite support.
- `deps/redismodule.h`: vendored Redis module API header.
- `tests/test_art.c`: standalone ART tests.
- `tests/test_module.rb`: integration tests against a temporary Redis server.

## Build And Test

Use these commands from the repository root:

```sh
make
timeout 60s make test
timeout 60s make test-module
```

`make test` does not require Redis. `make test-module` requires `redis-server`,
`redis-cli`, Ruby, and a built `rtree.so`.

The GitHub Actions workflow in `.github/workflows/ci.yml` mirrors this
validation on Ubuntu for pushes to `master`, pull requests, and manual runs.

Clean generated files with:

```sh
make clean
```

Do not commit generated artifacts:

- `rtree.so`
- `src/*.o`
- `tests/test_art`
- `.DS_Store`

## Coding Rules

- Keep the implementation in C99 and follow the existing Makefile flags.
- Prefer small, direct changes that match the current file ownership.
- Use Redis module allocation APIs for module-owned runtime data.
- Preserve Redis command arity, reply shapes, and error messages unless the task
  explicitly changes command behavior.
- Do not add compatibility shims or dead legacy paths for removed behavior.
- Keep comments in English and add them only when they explain non-obvious
  ownership, traversal, or Redis API behavior.

## Memory And Ownership

- `RTree` owns its `ArtTree`.
- Stored values are `RedisModuleString *` objects owned by the `RTree`.
- Replaced or deleted values must be freed exactly once.
- `art_set_allocator` is configured to Redis allocators when the module loads;
  standalone ART tests use the default allocator path.
- When buffering scan fields, copy field bytes and release the buffer on all
  error and success paths.

## Command Behavior

Registered commands:

- `RTREE.SET`
- `RTREE.GET`
- `RTREE.DEL`
- `RTREE.EXISTS`
- `RTREE.LEN`
- `RTREE.INFO`
- `RTREE.GETALL`
- `RTREE.KEYS`
- `RTREE.VALS`
- `RTREE.RANGE`
- `RTREE.REVRANGE`
- `RTREE.GETPREFIX`
- `RTREE.SCAN`

When changing command behavior, update all relevant places in the same change:

- command implementation in `src/rtree.c`
- command registration flags in `src/module.c`, when needed
- module integration coverage in `tests/test_module.rb`
- public command documentation in `README.md`

## Persistence

The Redis data type is named `rtree-art`.

- RDB save writes the field count followed by ordered field-value pairs.
- RDB load rebuilds a new `RTree` from the serialized pairs.
- AOF rewrite emits `RTREE.SET` commands.

Any persistence format change must include an encoding-version plan and tests.

## Review Checklist

- Build succeeds with `make`.
- `timeout 60s make test` passes.
- `timeout 60s make test-module` passes when Redis tools are installed.
- README and integration tests match any public command behavior changes.
- No generated build outputs are included in the diff.
