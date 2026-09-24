# cpp-kv-store

A Linux-focused, high-performance in-memory key-value server built in C++20. The project is designed
to demonstrate nonblocking TCP networking, `epoll`, concurrency, sharded locking, TTL expiration,
testing, sanitizers, and performance engineering without attempting to reimplement all of Redis.

Milestone 0 is complete: the development environment, build, smoke test, specifications, and staged
implementation plan are ready. The server itself begins in Milestone 1.

## Architecture Target

```text
TCP clients -> epoll event loop -> RESP parser -> bounded work queue
                                                   |
                                             worker pool
                                                   |
                                         sharded key-value store
                                                   |
                                              TTL manager
```

The target protocol is a RESP2-compatible subset supporting `PING`, `SET`, `GET`, `DEL`, `EXISTS`,
`EXPIRE`, and `TTL`.

## macOS Prerequisites

The source stays on your Mac, while compilation and testing happen in an Ubuntu Linux container.
This is necessary because the planned server uses Linux `epoll`; macOS uses `kqueue` instead.

Install or verify:

1. Docker Desktop is installed and open.
2. Apple's command-line tools are available: `xcode-select -p`.
3. Git is available: `git --version`.
4. Docker works: `docker run hello-world`.
5. Compose works: `docker compose version`.

You can check the host prerequisites with:

```bash
./scripts/check-host.sh
```

You do not need to install GCC, Clang, CMake, GoogleTest, Redis Server, or Valgrind directly on
macOS. The Docker image contains the project toolchain. `redis-cli` is included in the container for
future compatibility tests.

## First Setup

Unzip the project, open Terminal, and enter the directory:

```bash
cd /path/to/cpp-kv-store
```

Initialize local version control if the extracted copy does not already contain `.git`:

```bash
git init
git add .
git commit -m "Initialize C++ key-value store scaffold"
```

Build the Linux development image:

```bash
docker compose build dev
```

Run the starter build and test:

```bash
docker compose run --rm dev ./scripts/test.sh debug
```

Run the starter executable:

```bash
docker compose run --rm dev ./build/debug/kvstore_server --version
```

Expected output:

```text
cpp-kv-store 0.1.0
```

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
```

Or run each command from macOS without entering the shell:

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

Verified through Milestone 7 on Docker Desktop for Apple Silicon (Ubuntu 24.04 container, `arm64`,
Clang 18.1.3):

- The base `clang` package does **not** ship the sanitizer runtimes on this platform; ASan/UBSan
  failed to link (`cannot find libclang_rt.asan-aarch64.a`) until `libclang-rt-18-dev` was added to
  `Dockerfile`. If you rebuild the image from scratch and hit that link error, this is why.
- With that package present, `./scripts/test.sh asan` (ASan+UBSan) and `./scripts/test.sh tsan` both
  run cleanly end to end. Every milestone's test suite (172+ cases as of Milestone 6, including the
  concurrency-heavy storage, worker pool, and event loop tests) has passed under both with zero
  known project errors or data races.
- `cmake/Sanitizers.cmake` refuses a build that enables TSan together with ASan/UBSan in the same
  binary (they are not composable); the two are always run as separate `ctest` presets, per
  `CMakePresets.json`.
- No other environment-specific sanitizer limitations have been found on this platform. Benchmark
  numbers gathered under Docker Desktop are still not bare-metal results -- see "Important Benchmark
  Note" below -- but this does not affect sanitizer correctness, only timing.

## Docker Desktop Settings

Defaults are usually sufficient. A practical starting point is 4 CPU cores and 4–6 GB of memory,
depending on your Mac. Kubernetes is not needed. On Apple Silicon, use Docker's native ARM64 images;
do not force `linux/amd64` unless a verified dependency requires it.

## Repository Map

```text
cpp-kv-store/
├── AGENTS.md                 Codex repository instructions
├── SPEC.md                   Product behavior and acceptance criteria
├── ARCHITECTURE.md           Component and ownership design
├── TASKS.md                  Ordered implementation milestones
├── CODEX_START_PROMPT.md     Ready-to-paste first implementation prompt
├── Dockerfile                Ubuntu C++ development image
├── compose.yaml              Mac-to-Linux development workflow
├── CMakeLists.txt            Build and test entry point
├── CMakePresets.json         Debug, release, ASan, and TSan builds
├── cmake/                    Warning and sanitizer helpers
├── include/kvstore/          Public C++ headers
├── src/                      C++ implementation
├── tests/                    GoogleTest suite
├── benchmarks/               Benchmark plan and future client
├── scripts/                  Repeatable build/test helpers
└── docs/                     Protocol and concurrency design
```

## Development Order

Follow `TASKS.md` in order. The next step is Milestone 1, the thread-safe sharded storage engine.
Copy the prompt from `CODEX_START_PROMPT.md` into Codex after opening this folder as its workspace.

Do not ask Codex to implement the entire system in one pass. Each milestone should end with builds,
tests, and sanitizer verification before moving on.

## Important Benchmark Note

Docker Desktop is appropriate for correctness, integration tests, and development measurements.
Do not describe its performance numbers as bare-metal Linux results. Final portfolio benchmarking
should also run on a documented Linux host or VM.
