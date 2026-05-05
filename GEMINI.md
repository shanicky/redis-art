# Gemini Instructions

Use `AGENTS.md` as the source of truth for repository rules and implementation
guidance.

Repository summary:

- `rtree` is a C99 Redis module implementing an ordered hash over an adaptive
  radix tree.
- `README.md` is the public command reference.
- `src/rtree.c` contains command handlers and option parsing.
- `src/art.c` contains the ordered tree implementation.
- `tests/test_module.rb` verifies Redis-visible behavior.

Validation:

```sh
timeout 60s make test
timeout 60s make test-module
```

Do not commit generated outputs such as `rtree.so`, `src/*.o`, or
`tests/test_art`.
