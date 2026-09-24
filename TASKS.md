# Implementation Plan

Complete milestones in order. Every milestone ends with a clean build, passing tests, and updated
documentation. Check an item only after verifying it.

## Milestone 0 — Repository and Toolchain

- [x] Create repository layout, CMake build, Docker environment, and presets.
- [x] Add warning and sanitizer configuration.
- [x] Add a starter executable and GoogleTest smoke test.
- [x] Add specification, architecture, protocol, concurrency, and agent instructions.
- [x] Add a Linux CI workflow.
- [x] Verify debug, release, ASan/UBSan, and TSan starter builds on the development Mac.

## Milestone 1 — Thread-Safe Storage Engine

- [x] Define storage interface and command-independent result types.
- [x] Implement configurable sharded map with `std::shared_mutex`.
- [x] Implement `set`, `get`, `del`, `exists`, `expire`, and `ttl` semantics.
- [x] Use monotonic deadlines and entry generations.
- [x] Add deterministic clock injection for TTL tests.
- [x] Test replacement, missing keys, immediate expiry, and shard concurrency.
- [x] Pass debug, ASan/UBSan, and TSan tests.

## Milestone 2 — RESP Parser and Serializer

- [x] Define owned command representation.
- [x] Implement incremental RESP2 array/bulk-string parser.
- [x] Validate arity, integer arguments, and size limits.
- [x] Serialize simple strings, errors, integers, bulk strings, and null bulk strings.
- [x] Test every split point of representative frames and multiple pipelined frames.
- [x] Fuzz or property-test malformed lengths and truncated input.

## Milestone 3 — TCP Server Foundation

- [x] Add RAII wrappers for file descriptors.
- [x] Create configurable nonblocking listener.
- [x] Accept, register, and close clients safely.
- [x] Add connection IDs/generations and bounded buffers.
- [x] Test connect/disconnect and basic request/response on loopback.

## Milestone 4 — `epoll` Event Loop

- [x] Implement edge- or level-triggered behavior with one documented choice.
- [x] Handle partial reads/writes and `EAGAIN` correctly.
- [x] Parse multiple commands per read.
- [x] Add event-loop wakeup for completions and shutdown.
- [x] Test fragmented and pipelined requests.

## Milestone 5 — Worker Pool and Dispatch

- [x] Implement bounded blocking work and completion queues.
- [x] Implement fixed-size `std::jthread` pool.
- [x] Dispatch all seven commands to storage.
- [x] Preserve response order per connection.
- [x] Apply input/output backpressure and in-flight limits.
- [x] Test queue closure and out-of-order worker completion.

## Milestone 6 — Active TTL Expiration

- [x] Implement bounded background or event-loop cleanup.
- [x] Ignore stale expiration records using generation checks.
- [x] Test expiration/replacement races with an injectable clock.
- [x] Record expiration counters for later metrics.

## Milestone 7 — Reliability and Stress Tests

- [x] Add 1,000-connection integration test.
- [x] Add randomized concurrent operation test with a reference model.
- [x] Add graceful shutdown tests with active clients.
- [x] Run full ASan/UBSan suite.
- [x] Run TSan concurrency suite with zero known project races.
- [x] Document any environment-specific sanitizer limitations accurately.

## Milestone 8 — Benchmarking

- [x] Implement benchmark client or integrate a suitable open tool.
- [x] Report throughput, p50, p95, and p99 latency.
- [x] Run the workload matrix in `benchmarks/README.md` (representative subset; see
      `benchmarks/results/summary.md` for exactly what was and wasn't run, and why).
- [x] Record full environment and methodology.
- [x] Save raw results separately from summarized results.

## Milestone 9 — Profiling and Optimization

- [x] Profile a release build before changing code.
- [x] Identify measured hot paths and contention.
- [x] Optimize only evidence-backed bottlenecks (see `benchmarks/results/milestone9_profiling.md`).
- [x] Re-run correctness, sanitizer, and benchmark suites.
- [x] Compare before/after results without hiding regressions.

## Milestone 10 — Portfolio Release

- [x] Complete README usage, design, limitations, and benchmark sections.
- [x] Add an architecture diagram and a reproducible demo (`scripts/demo.sh`).
- [x] Verify `redis-cli` compatibility for supported commands.
- [x] Tag `v1.0.0` only after all acceptance criteria pass.

## Version 2 Backlog

- Append-only persistence and recovery
- Snapshotting
- LRU/LFU eviction and memory limits
- Authentication or TLS via a proven external library
- Additional RESP commands
