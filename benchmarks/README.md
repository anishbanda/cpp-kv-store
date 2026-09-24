# Benchmarks

Benchmark implementation begins in Milestone 8. Do not publish invented results.

The final benchmark must report throughput and p50, p95, and p99 latency for:

- 1, 10, 100, and 1,000 client connections
- 1, 2, 4, 8, and 16 worker threads where the host permits
- 100% GET, 100% SET, 80/20 GET/SET, and 50/50 GET/SET workloads

Record the CPU architecture, operating system, container/runtime status, compiler, build type,
payload size, keyspace size, test duration, and number of repetitions. Docker Desktop results are
valid development measurements but must not be described as bare-metal Linux results.

