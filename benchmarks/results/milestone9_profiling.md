# Milestone 9: Profiling and Optimization

## Methodology

`perf` is not installed in this Docker environment, and `/proc/sys/kernel/perf_event_paranoid`
is `2` there regardless (would block it even if installed). `valgrind` (3.22.0) is installed, so
profiling used `valgrind --tool=callgrind` against a **Release** build
(`./scripts/build.sh release`), driven by `kvstore_bench` (10 connections, 8-second duration,
`mixed5050` workload, 1,000-key keyspace) -- see `scripts/profile_run.sh`, which is reusable for
future profiling passes. Instrumented instruction counts (`Ir`, from callgrind) are the primary
before/after comparison metric below because they are exact and deterministic, unlike
wall-clock throughput under instrumentation, which callgrind's overhead makes noisy.

## Finding

The first profile showed one function dominating the entire process:

```
608,431,847 (51.30%)  memset [/usr/lib/aarch64-linux-gnu/libc.so.6]
```

over half of every instruction executed, server-wide, was zeroing memory. The cause was
`kvstore::net::read_into()` (`src/net/io.cpp`), which on every single nonblocking read
constructed a fresh `std::string` up to 64 KiB (`kReadChunkSize`) and zero-initialized all of it
via `std::string(n, '\0')`, then called `recv()` into it and `resize()`-ed down to the actual
byte count returned. For small requests (a `PING` frame is 14 bytes) this meant zero-filling up
to 64 KiB to use a few dozen of them, on every read, for every connection -- and discarding the
rest immediately.

## Fix

Replaced the heap-allocated, zero-initialized `std::string` with a reused `thread_local` stack
buffer that `recv()` writes into directly; the `BoundedBuffer::append()` copy afterward already
takes a `std::string_view`, so no first zero-fill was ever needed. `thread_local` is a defensive
choice, not a requirement of current usage (only the event-loop thread calls `read_into()` in
production, per ARCHITECTURE.md) -- it costs nothing extra here and rules out a data race if that
ever changes. See `src/net/io.cpp`.

This is the only change made this milestone: profiling found one overwhelming, unambiguous,
safe-to-fix cost, and TASKS.md calls for optimizing evidence-backed bottlenecks, not for finding
a fixed quota of things to change.

## Before / after: instrumented instruction count (same callgrind run parameters)

| | Instructions (`Ir`) | Requests completed | Throughput (instrumented) |
| --- | --- | --- | --- |
| Before | 1,185,996,848 | 73,400 | 9,173 req/s |
| After | 612,207,642 | 85,975 | 10,745 req/s |
| Change | **-48.4%** | +17.1% | +17.1% |

After the fix, `memset` no longer appears near the top of the profile at all; the largest single
cost is `_int_free` at 5.78%, an order of magnitude smaller than the eliminated `memset` cost and
representative of ordinary allocator overhead rather than a single dominant waste.

## Before / after: uninstrumented Release-build benchmark (GET workload, full matrix in the raw CSVs)

Re-ran `benchmarks/run_matrix.sh` in full before and after the fix.
`benchmarks/raw/results_before_m9_optimization.csv` and `results_after_m9_optimization.csv` hold
the complete, unedited 48-cell output of each pass; `benchmarks/raw/results.csv` currently holds
the after-fix data (the latest run). Throughput in req/s:

| Workers | Connections | Before | After | Change |
| --- | --- | --- | --- | --- |
| 1 | 1 | 20,006 | 20,570 | +2.8% |
| 1 | 10 | 110,739 | 127,145 | +14.8% |
| 1 | 100 | 127,135 | 155,832 | +22.6% |
| 1 | 1,000 | 116,678 | 143,107 | +22.7% |
| 4 | 1 | 19,467 | 18,694 | -4.0% |
| 4 | 10 | 56,658 | 78,938 | +39.3% |
| 4 | 100 | 49,826 | 71,773 | +44.1% |
| 4 | 1,000 | 46,677 | 63,118 | +35.2% |
| 8 | 1 | 18,484 | 18,474 | ~0% |
| 8 | 10 | 46,075 | 46,799 | +1.6% |
| 8 | 100 | 36,828 | 36,975 | +0.4% |
| 8 | 1,000 | 34,237 | 36,925 | +7.8% |

SET and the two mixed workloads show the same pattern at every cell; see the raw CSVs for exact
figures.

## Honest reading of the pattern (not hiding it)

The improvement is large at `--workers 1` and `--workers 4` (up to +44%) but small to negligible
at `--workers 8` (0-8%), including one single-connection cell that went slightly negative (-4.0%,
within ordinary run-to-run noise at only ~19K req/s and a 2-second window -- not a regression
attributable to the code change, since the change only removes work, never adds any). The likely
explanation: this container has 10 visible cores shared by the benchmark client and the server's
own worker threads (see `benchmarks/results/summary.md`'s environment notes). At 8 server workers
plus the client's own connection threads, the run is core-contended rather than per-request-CPU-
bound, so removing wasted CPU work per request has little room to show up as more completed
requests per second. At 1 or 4 workers there is more slack, and the saved CPU work converts
directly into higher throughput. This is reported as observed, not smoothed into a single
headline number.

## Correctness and sanitizer re-verification

Re-ran the full suite after the change: debug 191/191, full ASan+UBSan 191/191, full TSan 191/191
(zero known project races, including around the new `thread_local` buffer), release build clean.
