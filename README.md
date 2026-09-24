# cpp-kv-store

A Linux-focused, multithreaded in-memory key-value server in C++20, demonstrating nonblocking TCP
networking (`epoll`), sharded concurrency, TTL expiration, a RESP2-compatible wire protocol,
GoogleTest + sanitizer-driven testing, and evidence-based performance engineering. It is a focused
Redis-compatible subset (SPEC.md), not an attempt to reimplement Redis.

**Status: all ten `TASKS.md` milestones are complete (v1.0.0).** The server runs, is
`redis-cli`-compatible for its seven commands, is covered by 191 GoogleTest cases (clean under
debug, ASan+UBSan, and TSan), and has been benchmarked and profiled. See "Limitations" below for
what is deliberately out of scope.

## Supported Commands

| Command | Behavior |
| --- | --- |
| `PING` | Simple string `PONG` |
| `SET key value` | Store/replace a value; clears any existing TTL |
| `GET key` | Bulk string value, or a null bulk string if missing/expired |
| `DEL key` | Integer `1` if a live key was removed, else `0` |
| `EXISTS key` | Integer `1` if live, else `0` |
| `EXPIRE key seconds` | Integer `1` if the TTL was set, else `0`; `0` seconds expires immediately |
| `TTL key` | Remaining whole seconds, `-1` if no TTL, `-2` if missing |

Full wire-level detail is in `docs/PROTOCOL.md`; full behavioral detail is in `SPEC.md`.

## Architecture

```text
TCP clients
    |
    v
TCP listener -> epoll event loop -> per-connection RESP parser
 (nonblocking,                        (incremental, pipelining-aware)
  Milestone 3)                              |
                                             v
                                     bounded work queue
                                             |
                                      worker thread pool  <--- kvstore::command::dispatch
                                     (std::jthread, N)           |
                                             |                   v
                                             |          sharded key-value store
                                             |         (shared_mutex per shard,
                                             |          generation-checked TTL)
                                             |                   ^
                                             |                   |
                                             |         TTL sweeper (background std::jthread,
                                             |          bounded per-shard batches)
                                             v
                                    bounded completion queue
                                             |
                                             v
                              epoll event loop -> clients
                          (per-connection response ordering via
                           ResponseSequencer, even though workers
                           may finish out of order)
```

One event-loop thread owns all socket I/O and connection state; a fixed worker pool executes
parsed commands against the sharded store; a background sweeper actively expires keys, generation-
checked against concurrent replacement so a stale expiration can never delete a newer value. Full
component boundaries and ownership rules are in `ARCHITECTURE.md`; the concurrency/locking model
and shutdown order are in `docs/CONCURRENCY.md`.

## macOS Prerequisites

The source stays on your Mac; compilation and testing happen in an Ubuntu Linux container, because
the server uses Linux `epoll` (macOS uses `kqueue` instead).

Install or verify:

1. Docker Desktop is installed and open.
2. Apple's command-line tools are available: `xcode-select -p`.
3. Git is available: `git --version`.
4. Docker works: `docker run hello-world`.
5. Compose works: `docker compose version`.

```bash
./scripts/check-host.sh
```

You do not need GCC, Clang, CMake, GoogleTest, Valgrind, or `redis-cli` on macOS itself -- the
Docker image contains the whole toolchain, including `redis-cli` for compatibility checks.

## Quick Start

```bash
docker compose build dev
docker compose run --rm dev ./scripts/test.sh debug     # build + run all 191 tests
docker compose run --rm dev ./scripts/build.sh release
docker compose run --rm dev ./scripts/demo.sh            # start the server, drive it with redis-cli
```

`scripts/demo.sh` is a reproducible, self-contained walkthrough: it starts a real
`kvstore_server`, exercises every command via `redis-cli` (including TTL/expiration, deletion, and
an unknown-command error that leaves the connection healthy), then sends `SIGTERM` and shows
graceful shutdown logging.

To run the server yourself and connect manually, `docker compose exec` needs a persistent service
container (`docker compose run` creates a separate, one-off container each time), so start one
with `up -d` first:

```bash
docker compose up -d dev
docker compose exec -d dev ./build/release/kvstore_server --port 6380
docker compose exec dev redis-cli -p 6380 PING
docker compose exec dev redis-cli -p 6380 SET demo works
docker compose exec dev redis-cli -p 6380 GET demo
docker compose exec dev pkill -TERM kvstore_server   # graceful shutdown
docker compose down
```

(`compose.yaml` also publishes container port 6380 to the host, so `redis-cli -p 6380` from your
Mac, if you have `redis-cli` installed there, works too, once the server above is running.)

### CLI Options

```text
Usage: kvstore_server [options]

Options:
  --bind <address>        Bind address (default: 0.0.0.0)
  --port <port>           Listen port (default: 6380)
  --workers <count>       Worker thread count (default: 4)
  --shards <count>        Storage shard count, rounded up to a power of two (default: 32)
  --queue-capacity <n>    Work/completion queue capacity (default: 1024)
  --log-level <level>     debug | info | warn | error (default: info)
  --help                  Print this message and exit
  --version               Print the version and exit
```

`SIGINT`/`SIGTERM` trigger graceful shutdown: the event loop stops accepting new I/O, the worker
pool finishes already-queued work before its threads exit, and the process logs shutdown start and
completion before exiting 0 (`docs/CONCURRENCY.md`, "Shutdown Order").

## Common Commands

Open a Linux shell with the repository mounted at `/workspace`:

```bash
./scripts/shell.sh
```

From inside that shell:

```bash
./scripts/test.sh debug
./scripts/test.sh asan
./scripts/test.sh tsan
./scripts/build.sh release
./scripts/format.sh
./scripts/demo.sh
./benchmarks/run_matrix.sh
./scripts/profile_run.sh
```

Or run each command from macOS without entering the shell, e.g.:

```bash
docker compose run --rm dev ./scripts/test.sh debug
docker compose run --rm dev ./scripts/test.sh asan
docker compose run --rm dev ./scripts/test.sh tsan
docker compose run --rm dev ./scripts/build.sh release
```

To keep a development container running in the background:

```bash
docker compose up -d dev
docker compose exec dev bash
docker compose down
```

The bind mount `.:/workspace` means code edited on your Mac is immediately visible inside the
container. Containers are disposable; your source remains in the Mac project folder.

## Sanitizer Environment Notes

Verified on Docker Desktop for Apple Silicon (Ubuntu 24.04 container, `arm64`, Clang 18.1.3),
consistently from Milestone 1 through Milestone 9 (191 test cases as of v1.0.0):

- The base `clang` package does **not** ship the sanitizer runtimes on this platform; ASan/UBSan
  failed to link (`cannot find libclang_rt.asan-aarch64.a`) until `libclang-rt-18-dev` was added to
  `Dockerfile`. If you rebuild the image from scratch and hit that link error, this is why.
- With that package present, `./scripts/test.sh asan` (ASan+UBSan) and `./scripts/test.sh tsan`
  both run cleanly end to end, every milestone, with zero known project errors or data races.
- `cmake/Sanitizers.cmake` refuses a build that enables TSan together with ASan/UBSan in the same
  binary (they are not composable); the two are always run as separate `ctest` presets.
- `perf` is not installed in this container, and `/proc/sys/kernel/perf_event_paranoid` is `2`
  there regardless (would block it even if installed); Milestone 9 profiling used
  `valgrind --tool=callgrind` instead (see `scripts/profile_run.sh`).
- No other environment-specific sanitizer limitations have been found. Docker Desktop numbers
  (sanitizer or benchmark) are development-environment measurements, not bare-metal Linux results
  -- see "Important Benchmark Note" below.

## Docker Desktop Settings

Defaults are usually sufficient. A practical starting point is 4 CPU cores and 4-6 GB of memory,
depending on your Mac. Kubernetes is not needed. On Apple Silicon, use Docker's native ARM64
images; do not force `linux/amd64` unless a verified dependency requires it.

## Benchmarks

See `benchmarks/results/summary.md` (Milestone 8 baseline: environment, methodology, throughput
and p50/p95/p99 latency across a representative subset of the connection-count x worker-count x
workload matrix from `benchmarks/README.md`) and `benchmarks/results/milestone9_profiling.md`
(profiling finding, the resulting fix, and a full before/after re-run of that matrix). Raw,
unedited output of every run is in `benchmarks/raw/`, kept separate from the summarized reports.
Never treat these as bare-metal Linux numbers or as a production capacity claim -- see "Important
Benchmark Note" below.

## Limitations (Version 1, by design -- SPEC.md section 9)

- No persistence: all data is lost on process exit. No append-only file, snapshotting, or crash
  recovery.
- No replication, clustering, consensus, or distributed locks.
- No transactions, Lua scripting, pub/sub, streams, or authentication.
- No TLS.
- No LRU/LFU eviction or hard memory quotas -- the store grows until the host is out of memory.
- Not full RESP3 or full Redis command compatibility: exactly the seven commands listed above,
  matching `redis-cli` for those, not a Redis replacement.
- IPv4 only (`ARCHITECTURE.md`'s portability boundary keeps core storage/protocol/concurrency code
  independent of `epoll` so a `kqueue` backend is possible later, but Version 1 implements only
  Linux `epoll`).
- Keys capped at 512 bytes, values at 1 MiB (`SPEC.md` section 4).
- Single-machine, shared-core Docker Desktop measurements only; see "Important Benchmark Note".

Persistence and eviction are Version 2 candidates (`TASKS.md`, "Version 2 Backlog") only after
Version 1 is correct and measured, which it now is.

## Repository Map

```text
cpp-kv-store/
├── AGENTS.md                 Repository instructions for coding agents
├── SPEC.md                   Product behavior and acceptance criteria
├── ARCHITECTURE.md           Component and ownership design
├── TASKS.md                  Ordered implementation milestones (all complete)
├── CLAUDE_START_PROMPT.md    The prompt that began Milestone 1 implementation
├── Dockerfile                Ubuntu C++ development image
├── compose.yaml              Mac-to-Linux development workflow
├── CMakeLists.txt            Build, test, and benchmark-client entry point
├── CMakePresets.json         Debug, release, ASan, and TSan build presets
├── cmake/                    Warning and sanitizer helpers
├── include/kvstore/          Public C++ headers (storage, protocol, net, worker, command, cli, util)
├── src/                      C++ implementation, plus main.cpp
├── tests/                    GoogleTest suite (191 cases), mirroring include/kvstore/'s layout
├── benchmarks/                Benchmark client, matrix runner, raw and summarized results
│   ├── client/                kvstore_bench, a standalone RESP2 client
│   ├── raw/                    Unedited CSV output of every benchmark run
│   └── results/                Human-written summaries and the Milestone 9 profiling report
├── scripts/                   Repeatable build/test/demo/profiling helpers
└── docs/                      RESP2 protocol subset and concurrency/locking design
```

## Development History

All ten milestones in `TASKS.md` are complete, in order, each gated on its own build, full test
suite, and (where applicable) sanitizer run before the next began. `TASKS.md` itself records
exactly which items were verified at each milestone; nothing there is checked off without having
actually been run.

## Important Benchmark Note

Docker Desktop is appropriate for correctness, integration tests, and development measurements.
Do not describe its performance numbers as bare-metal Linux results. Reproducing them on a
dedicated Linux host or VM would be the natural next step for a stricter capacity claim.
