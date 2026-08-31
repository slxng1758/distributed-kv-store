#!/usr/bin/env bash
# Starts kvserver, drives it via kvclient through GET/SET/DELETE, and
# asserts exact output at each step. This is the required correctness gate
# before trusting any benchmark run -- run_benchmark.sh runs this first and
# aborts if it fails.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
PORT="${SMOKE_TEST_PORT:-16380}"

SERVER_BIN="$BUILD_DIR/kvserver"
CLIENT_BIN="$BUILD_DIR/kvclient"

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
  echo "smoke_test: build kvserver and kvclient first (cmake --build $BUILD_DIR)" >&2
  exit 1
fi

"$SERVER_BIN" --port "$PORT" &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the server to start accepting connections.
ready=0
for _ in $(seq 1 50); do
  if "$CLIENT_BIN" --port "$PORT" GET __smoke_probe__ >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
  echo "smoke_test: server never came up on port $PORT" >&2
  exit 1
fi

FAILURES=0

check() {
  local desc="$1" expected="$2" actual="$3"
  if [[ "$actual" != "$expected" ]]; then
    echo "FAIL: $desc -- expected '$expected', got '$actual'"
    FAILURES=$((FAILURES + 1))
  else
    echo "OK: $desc"
  fi
}

out=$("$CLIENT_BIN" --port "$PORT" SET foo bar)
check "SET foo bar" "OK" "$out"

out=$("$CLIENT_BIN" --port "$PORT" GET foo)
check "GET foo" "bar" "$out"

out=$("$CLIENT_BIN" --port "$PORT" GET missing)
check "GET missing" "NOT_FOUND" "$out"

out=$("$CLIENT_BIN" --port "$PORT" DELETE foo)
check "DELETE foo" "DELETED" "$out"

out=$("$CLIENT_BIN" --port "$PORT" GET foo)
check "GET foo after delete" "NOT_FOUND" "$out"

out=$("$CLIENT_BIN" --port "$PORT" DELETE foo)
check "DELETE foo again" "NOT_FOUND" "$out"

out=$("$CLIENT_BIN" --port "$PORT" SET withspaces a value with spaces)
check "SET withspaces 'a value with spaces'" "OK" "$out"

out=$("$CLIENT_BIN" --port "$PORT" GET withspaces)
check "GET withspaces" "a value with spaces" "$out"

if [[ "$FAILURES" -gt 0 ]]; then
  echo "$FAILURES smoke check(s) failed"
  exit 1
fi

echo "smoke_test: all checks passed"
exit 0
