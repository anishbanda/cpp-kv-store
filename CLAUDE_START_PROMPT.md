# First Codex Implementation Prompt

Open this repository as the Codex workspace, confirm Docker Desktop is running, and give Codex the
prompt below. The repository scaffold already completes Milestone 0, so this prompt begins Milestone
1 rather than recreating existing files.

---

You are implementing Milestone 1 of this repository: the thread-safe in-memory storage engine.

First read `AGENTS.md`, then read `SPEC.md`, `ARCHITECTURE.md`, `TASKS.md`, and
`docs/CONCURRENCY.md`. Inspect the existing CMake and test setup before editing anything.

Implement Milestone 1 only. Do not start networking, RESP parsing, the worker pool, active TTL
cleanup, persistence, eviction, or benchmarking.

Required deliverables:

1. A clean storage API under `include/kvstore/storage/` and implementation under
   `src/storage/`.
2. A configurable sharded in-memory map using C++20 and `std::shared_mutex`.
3. Correct operations for `set`, `get`, `del`, `exists`, `expire`, and `ttl` as specified in
   `SPEC.md`.
4. Monotonic expiration deadlines and generation identifiers so later active-expiration work can
   reject stale records.
5. An injectable clock or equivalent deterministic seam; TTL unit tests must never rely on real
   sleeps.
6. Focused GoogleTest coverage for normal behavior, missing keys, replacement clearing TTL,
   immediate expiration, TTL return values, size boundaries, and concurrent access.
7. Necessary CMake updates and a concise update to `TASKS.md` showing only work actually verified.

Design constraints:

- Preserve the architecture and ownership rules in the documentation.
- A one-key operation must lock only one shard.
- Avoid global mutable state and owning raw pointers.
- Do not hold a lock while sleeping, logging, or invoking user-supplied code.
- Keep public APIs small and command/protocol independent.
- Reject invalid configuration, including zero shards; prefer a power-of-two shard count if it
  improves indexing, and document the choice.
- Use clear result types rather than Redis-formatted strings in the storage layer.
- Do not add third-party dependencies.
- Do not weaken warnings or remove existing tests.

Verification must run inside the Linux Docker environment. At minimum run:

```bash
docker compose build dev
docker compose run --rm dev ./scripts/test.sh debug
docker compose run --rm dev ./scripts/test.sh asan
docker compose run --rm dev ./scripts/test.sh tsan
docker compose run --rm dev ./scripts/build.sh release
```

If a command fails, diagnose and fix the cause before proceeding. Do not claim a sanitizer passed
if the runtime itself is unsupported; report the exact limitation instead.

When finished, provide:

- a short design summary
- files changed
- tests added
- exact verification commands and results
- any remaining risks or decisions for Milestone 2

Do not create a git commit or push to a remote unless I explicitly ask.

---