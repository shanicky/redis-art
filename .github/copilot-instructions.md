# Copilot Instructions

Follow the repository guidance in `AGENTS.md`.

This repository is a C99 Redis module. Keep suggestions aligned with the
existing Redis Module API style, Makefile build, and ART-backed ordered hash
implementation.

When editing public command behavior, keep these files synchronized:

- `src/rtree.c`
- `src/module.c`
- `tests/test_module.rb`
- `README.md`

Preferred validation:

```sh
timeout 60s make test
timeout 60s make test-module
```
