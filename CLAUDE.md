# Claude Instructions

Follow `AGENTS.md` as the authoritative guide for this repository.

Important local context:

- This is a C99 Redis module named `rtree`.
- The module output is `rtree.so`.
- Public command behavior is documented in `README.md`.
- Command changes must keep `src/rtree.c`, `src/module.c`,
  `tests/test_module.rb`, and `README.md` in sync.

Before finishing code changes, run:

```sh
timeout 60s make test
```

Also run this when `redis-server` and `redis-cli` are available:

```sh
timeout 60s make test-module
```
