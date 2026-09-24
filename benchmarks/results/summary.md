# Benchmark Summary

**This is the Milestone 8 baseline, captured before Milestone 9's profiling-driven
optimization.** `benchmarks/raw/results.csv` has since been overwritten by that milestone's
post-optimization re-run; the exact before/after data (both preserved separately) and the
optimization itself are in `benchmarks/results/milestone9_profiling.md`. The methodology and
environment described below are otherwise unchanged and still apply to the current results.

**Do not treat these as bare-metal Linux numbers.** Everything below ran inside a single Docker
Desktop container on a MacBook (Apple Silicon host, `arm64` container), with the benchmark client
and the server sharing the same container and CPU cores. They are valid development-environment
measurements, not a production capacity claim. See `README.md`'s "Important Benchmark Note".

## Environment

| | |
| --- | --- |
| Host | Docker Desktop for Apple Silicon (macOS host, `arm64` container, not emulated) |
| Container OS | Ubuntu 24.04.5 LTS |
| Kernel | Linux 7.0.12-linuxkit (Docker Desktop's LinuxKit VM), `aarch64` |
| CPU cores visible to container | 10 (`nproc`) |
| Compiler | Clang 18.1.3 |
| Build type | Release (`./scripts/build.sh release`) -- `-O3`-class optimizations, no sanitizers |
| Server | `kvstore_server`, this repository, commit at time of Milestone 8 |
| Client | `kvstore_bench` (`benchmarks/client/kv_bench.cpp`), same binary tree, also Release |
| Transport | TCP over loopback (127.0.0.1), `TCP_NODELAY` set on client sockets |
| Payload size | 64-byte values (`--value-size 64`, the tool's default) |
| Keyspace size | 2,000 keys (`--keyspace 2000`), pre-populated via SET before each timed run |
| Per-run duration | 2 seconds (`--duration 2`) |
| Repetitions | 1 run per cell (not repeated/averaged -- see Limitations) |

## Methodology

- `benchmarks/run_matrix.sh` starts a fresh `kvstore_server` for each worker-count, runs every
  connection-count x workload combination against it with `kvstore_bench --csv`, then stops that
  server before moving to the next worker-count.
- `kvstore_bench` is closed-loop: each connection has at most one request in flight at a time,
  sending the next request only once the previous reply is fully read. This measures true
  per-request latency at the cost of not probing maximum pipelined throughput. Every connection
  pre-populates against a shared keyspace before the timed window starts.
- Throughput is `completed requests / actual wall-clock elapsed time` (not the nominal `--duration`,
  which the last in-flight request per connection can run slightly past). Latency percentiles are
  computed from every individual request's round-trip time across all connections in that run,
  sorted and indexed (nearest-rank method).
- Raw, unedited output of every one of the 48 runs below is in `benchmarks/raw/results.csv`. This
  summary is derived from that file; the file is the source of truth if the two ever disagree.

## Workload matrix coverage

TASKS.md's full matrix is 5 worker-counts x 4 connection-counts x 4 workloads = 80 cells. This run
covers a representative subset -- **3 worker-counts (1, 4, 8) x 4 connection-counts (1, 10, 100,
1,000) x 4 workloads = 48 cells** -- to keep total run time reasonable in a development container
shared with the benchmark client itself. Worker counts 2 and 16 were not run. This is disclosed
here rather than either skipping benchmarking or filling in unrun cells with estimates, which
SPEC.md and this file both explicitly forbid.

## Results: GET workload (representative; full data for all four workloads in the raw CSV)

| Workers | Connections | Throughput (req/s) | p50 (us) | p95 (us) | p99 (us) |
| --- | --- | --- | --- | --- | --- |
| 1 | 1 | 20,006 | 48.1 | 58.9 | 65.8 |
| 1 | 10 | 110,739 | 80.0 | 139.5 | 268.7 |
| 1 | 100 | 127,135 | 780.3 | 870.1 | 1,131.3 |
| 1 | 1,000 | 116,678 | 5,283.9 | 8,991.2 | 12,633.6 |
| 4 | 1 | 19,467 | 49.1 | 62.5 | 66.9 |
| 4 | 10 | 56,658 | 167.2 | 288.8 | 357.6 |
| 4 | 100 | 49,826 | 1,993.8 | 2,443.5 | 2,769.6 |
| 4 | 1,000 | 46,677 | 12,624.0 | 20,474.7 | 31,822.6 |
| 8 | 1 | 18,484 | 49.1 | 66.7 | 125.7 |
| 8 | 10 | 46,075 | 207.7 | 342.5 | 413.7 |
| 8 | 100 | 36,828 | 2,753.3 | 3,024.8 | 3,131.8 |
| 8 | 1,000 | 34,237 | 19,213.2 | 27,152.1 | 44,320.7 |

SET and the two mixed (80/20, 50/50 GET/SET) workloads tracked the GET workload closely at every
cell (within roughly 5% on throughput, generally lower p50 for GET as expected since SET does
slightly more work) -- see the raw CSV for exact figures.

## Observations

- **More worker threads did not increase throughput here, and often reduced it.** `--workers 1`
  outperforms `--workers 4` and `--workers 8` at every connection count in this run. The
  container has 10 visible cores, but the benchmark client and server share that same pool; at
  higher connection counts the client's own connection-handling threads are competing for the
  same cores as the server's workers, and PING/GET/SET are cheap enough (a single shard-mutex
  hash lookup) that added worker parallelism does not offset that contention. This is a
  measured result, not a hypothesis -- see Milestone 9 for whether it changes with a
  server-only profiling setup.
- **Latency grows with connection count, as expected for a single-core-bound event loop.** The
  event loop (Milestone 4) is one thread; more concurrent connections mean more queueing before
  each gets its turn, visible directly in the p50 column growing roughly 100x from 1 to 1,000
  connections.
- **Zero errors across all 48 runs**, including at 1,000 concurrent connections, consistent with
  Milestone 7's dedicated 1,000-connection reliability test.

## Limitations of this run

- Single-machine, shared-core client+server measurement, not an isolated network benchmark.
- One run per cell, not repeated/averaged; the numbers above have ordinary run-to-run variance
  that repetition would quantify but this pass did not measure.
- `kvstore_bench` is closed-loop (one outstanding request per connection); it does not measure
  maximum pipelined throughput, which a saturating open-loop or pipelined client would show as
  higher than the numbers here.
- Worker counts 2 and 16, and the full 80-cell matrix, were not run (see "Workload matrix
  coverage" above).
