#!/usr/bin/env bash
# Orchestrates a full, reproducible Phase 1 benchmark run: build -> smoke
# test (abort on failure, so a broken server never produces numbers) ->
# start server under `time -l` -> run kvbench with its standard-workload
# defaults -> stop server -> report where the results landed.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
PORT="${KVBENCH_PORT:-6380}"

echo "=== Building ==="
cmake -S . -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

echo "=== Running unit tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "=== Running smoke test ==="
export BUILD_DIR
if ! ./tests/smoke_test.sh; then
  echo "Smoke test failed -- aborting before benchmarking a broken server." >&2
  exit 1
fi

mkdir -p results
RUN_ID="$(date +%Y%m%d-%H%M%S)"
RUSAGE_FILE="results/${RUN_ID}.server_rusage.txt"

echo "=== Starting server for benchmark ==="
/usr/bin/time -l "$BUILD_DIR/kvserver" --port "$PORT" >/tmp/kvserver_bench.log 2>"$RUSAGE_FILE" &
TIME_PID=$!

cleanup() {
  # `time` wraps kvserver as a child. Kill kvserver specifically and let
  # `time` observe the exit itself (via its own wait4()) so it still gets
  # to write the rusage stats -- killing the `time` process directly races
  # with that write and can leave the rusage file empty.
  local server_child
  server_child="$(pgrep -P "$TIME_PID" 2>/dev/null || true)"
  if [[ -n "$server_child" ]]; then
    kill "$server_child" 2>/dev/null || true
  else
    kill "$TIME_PID" 2>/dev/null || true
  fi
  wait "$TIME_PID" 2>/dev/null || true
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 50); do
  if "$BUILD_DIR/kvclient" --port "$PORT" GET __bench_probe__ >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
done
if [[ "$ready" -ne 1 ]]; then
  echo "Server never came up on port $PORT" >&2
  exit 1
fi

echo "=== Running benchmark (standard workload: 10000 requests, 50 connections, 70/30 GET/SET) ==="
"$BUILD_DIR/kvbench" --port "$PORT" --output "results/${RUN_ID}.json"

echo
echo "Done."
echo "  Results:       results/${RUN_ID}.json"
echo "  Server rusage: ${RUSAGE_FILE}"
