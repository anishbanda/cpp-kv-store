#!/usr/bin/env bash
# A reproducible, self-contained demo of the running server (Milestone 10).
# Starts a real kvstore_server, drives it with redis-cli exactly as any
# RESP2 client would, then demonstrates graceful shutdown. Run from the
# repository root inside the dev container:
#
#   ./scripts/build.sh release   # once, if not already built
#   ./scripts/demo.sh
set -uo pipefail

SERVER_BIN="./build/release/kvstore_server"
PORT=6399

if [[ ! -x "$SERVER_BIN" ]]; then
  echo "error: ${SERVER_BIN} not found. Run ./scripts/build.sh release first." >&2
  exit 1
fi

step() {
  echo
  echo "\$ redis-cli -p ${PORT} $*"
  redis-cli -p "$PORT" "$@"
}

echo "=== starting kvstore_server on port ${PORT} ==="
"$SERVER_BIN" --port "$PORT" --workers 4 --log-level info &
SERVER_PID=$!
sleep 0.5

echo
echo "=== basic commands ==="
step PING
step SET greeting "hello from cpp-kv-store"
step GET greeting
step EXISTS greeting
step TTL greeting

echo
echo "=== TTL / expiration ==="
step EXPIRE greeting 100
step TTL greeting
step SET greeting "replacing clears any TTL"
step TTL greeting

echo
echo "=== immediate expiration ==="
step SET temp "gone soon"
step EXPIRE temp 0
step GET temp
step EXISTS temp

echo
echo "=== deletion ==="
step SET to-delete "bye"
step DEL to-delete
step DEL to-delete
step GET to-delete

echo
echo "=== unknown command: a clean RESP error, not a crash or a dropped connection ==="
step FOOBAR somearg
step PING  # a fresh redis-cli connection, but proves the server is still healthy

echo
echo "=== graceful shutdown (SIGTERM) ==="
kill -TERM "$SERVER_PID"
wait "$SERVER_PID"
echo "server exited with code $?"
