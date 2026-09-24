#!/usr/bin/env bash
# Runs a representative subset of the workload matrix from
# benchmarks/README.md against a release build of kvstore_server, using
# kvstore_bench. Run from the repository root inside the dev container
# (a release build must already exist: ./scripts/build.sh release).
#
# Writes raw, unedited CSV rows to benchmarks/raw/results.csv (one line
# per run: workers,connections,workload,throughput_rps,p50_us,p95_us,
# p99_us,requests,errors,duration_s). Never hand-edit that file -- it is
# the record of what was actually run. A separate, human-written summary
# belongs in benchmarks/results/summary.md.
set -euo pipefail

cd "$(dirname "$0")/.."

SERVER_BIN="./build/release/kvstore_server"
BENCH_BIN="./build/release/kvstore_bench"
PORT=6392
DURATION_SECONDS="${DURATION_SECONDS:-2}"
KEYSPACE="${KEYSPACE:-2000}"
RAW_FILE="benchmarks/raw/results.csv"

WORKER_COUNTS=(1 4 8)
CONNECTION_COUNTS=(1 10 100 1000)
WORKLOADS=(get set mixed8020 mixed5050)

ulimit -n 8192 2>/dev/null || true

echo "workers,connections,workload,throughput_rps,p50_us,p95_us,p99_us,requests,errors,duration_s" > "$RAW_FILE"

for workers in "${WORKER_COUNTS[@]}"; do
  echo "=== starting server with --workers ${workers} ===" >&2
  "$SERVER_BIN" --port "$PORT" --workers "$workers" --log-level error &
  SERVER_PID=$!
  sleep 0.5

  for connections in "${CONNECTION_COUNTS[@]}"; do
    for workload in "${WORKLOADS[@]}"; do
      echo "workers=${workers} connections=${connections} workload=${workload}" >&2
      LINE=$("$BENCH_BIN" --port "$PORT" --connections "$connections" --duration "$DURATION_SECONDS" \
        --workload "$workload" --keyspace "$KEYSPACE" --csv)
      echo "${workers},${LINE}" >> "$RAW_FILE"
    done
  done

  kill -TERM "$SERVER_PID"
  wait "$SERVER_PID" 2>/dev/null || true
done

echo "done: raw results in ${RAW_FILE}" >&2
