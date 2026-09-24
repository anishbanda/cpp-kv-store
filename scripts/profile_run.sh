#!/usr/bin/env bash
# Profiles a release-build kvstore_server under valgrind's callgrind tool
# (perf is unavailable in this container and perf_event_paranoid=2 would
# block it regardless). Run from the repository root inside the dev
# container after ./scripts/build.sh release.
set -uo pipefail

cd "$(dirname "$0")/.."
PROFILE_DIR="$(pwd)/.profile_tmp"
mkdir -p "$PROFILE_DIR"
cd "$PROFILE_DIR"
rm -f callgrind.out.*

valgrind --tool=callgrind --callgrind-out-file=callgrind.out.%p \
  /workspace/build/release/kvstore_server --port 6393 --log-level error &
CALLGRIND_PID=$!
sleep 3  # let the server (and valgrind's own startup) settle before load starts

/workspace/build/release/kvstore_bench --port 6393 --connections 10 --duration 8 \
  --workload mixed5050 --keyspace 1000

kill -TERM "$CALLGRIND_PID"
wait "$CALLGRIND_PID" 2>/dev/null || true

OUT_FILE=$(ls -1 callgrind.out.* | head -1)
echo "--- profile file: ${OUT_FILE} ---"
echo "--- callgrind_annotate (top of the sorted-by-cost function list) ---"
callgrind_annotate "$OUT_FILE" 2>&1 | head -70
