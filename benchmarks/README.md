# Benchmarks

Do not publish invented results (SPEC.md section 10: "Results are generated from actual runs and
are never hard-coded or estimated").

- `client/kv_bench.cpp` -- a small standalone RESP2 client (`kvstore_bench` in the CMake build),
  reporting throughput and p50/p95/p99 latency. Not linked against `kvstore_core`: it speaks the
  wire protocol directly, exercising the server exactly as any external client would.
- `run_matrix.sh` -- runs `kvstore_bench` across a workload matrix against a fresh
  `kvstore_server` per worker-count, writing raw CSV to `raw/results.csv`.
- `raw/` -- unedited output of every benchmark run. Never hand-edited.
- `results/summary.md` -- the human-readable report: environment, methodology, a results table,
  and honestly-disclosed limitations (including exactly which cells of the matrix below were and
  were not run, and why).

The full matrix reports throughput and p50, p95, and p99 latency for:

- 1, 10, 100, and 1,000 client connections
- 1, 2, 4, 8, and 16 worker threads where the host permits
- 100% GET, 100% SET, 80/20 GET/SET, and 50/50 GET/SET workloads

`results/summary.md` records exactly which subset of this matrix was actually run in the current
results (Milestone 8 covered a representative subset, not all 80 cells, to keep run time
reasonable in a shared development container -- disclosed there rather than invented).

Record the CPU architecture, operating system, container/runtime status, compiler, build type,
payload size, keyspace size, test duration, and number of repetitions. Docker Desktop results are
valid development measurements but must not be described as bare-metal Linux results.

To reproduce (inside the dev container, after `./scripts/build.sh release`):

```bash
./benchmarks/run_matrix.sh
```

