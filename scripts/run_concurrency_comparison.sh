#!/usr/bin/env bash
# Phase 2's headline metric: build once, then run the exact same standard
# benchmark workload against the SAME kvserver binary at --threads 1 (which
# reproduces Phase 1's single-threaded behavior) and --threads N, so the
# comparison isolates the concurrency variable instead of conflating it
# with any other code change between phases.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
PORT="${KVBENCH_PORT:-6380}"
CONCURRENT_THREADS="${CONCURRENT_THREADS:-4}"
# Extra kvbench flags, e.g. to reproduce the heavier-workload result noted
# in docs/CONCURRENCY.md where concurrency actually pays off:
#   KVBENCH_EXTRA_ARGS="--requests 200000 --value-size 65536" CONCURRENT_THREADS=2 ./scripts/run_concurrency_comparison.sh
KVBENCH_EXTRA_ARGS="${KVBENCH_EXTRA_ARGS:-}"

echo "=== Building ==="
cmake -S . -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

echo "=== Running unit tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

export BUILD_DIR
echo "=== Running smoke test ==="
if ! ./tests/smoke_test.sh; then
  echo "Smoke test failed -- aborting before benchmarking a broken server." >&2
  exit 1
fi

mkdir -p results
RUN_ID="$(date +%Y%m%d-%H%M%S)"

# Runs the standard workload against kvserver started with the given
# --threads value; prints the resulting JSON results path on stdout (all
# progress output goes to stderr so the caller can safely capture just the
# path via command substitution).
run_one() {
  local threads="$1" label="$2"
  local rusage_file="results/${RUN_ID}-${label}.server_rusage.txt"
  local json_file="results/${RUN_ID}-${label}.json"

  /usr/bin/time -l "$BUILD_DIR/kvserver" --port "$PORT" --threads "$threads" \
    >/tmp/kvserver_bench.log 2>"$rusage_file" &
  local time_pid=$!

  cleanup_one() {
    local server_child
    server_child="$(pgrep -P "$time_pid" 2>/dev/null || true)"
    if [[ -n "$server_child" ]]; then
      kill "$server_child" 2>/dev/null || true
    else
      kill "$time_pid" 2>/dev/null || true
    fi
    wait "$time_pid" 2>/dev/null || true
  }
  trap cleanup_one RETURN

  local ready=0
  for _ in $(seq 1 50); do
    if "$BUILD_DIR/kvclient" --port "$PORT" GET __bench_probe__ >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.1
  done
  if [[ "$ready" -ne 1 ]]; then
    echo "Server (threads=$threads) never came up on port $PORT" >&2
    return 1
  fi

  echo "=== Running benchmark: threads=$threads ===" >&2
  # shellcheck disable=SC2086
  "$BUILD_DIR/kvbench" --port "$PORT" --output "$json_file" $KVBENCH_EXTRA_ARGS >&2
  echo "$json_file"
}

echo
echo ">>> Baseline: --threads 1 (Phase 1-equivalent single-threaded behavior)"
BASELINE_JSON=$(run_one 1 "threads1")

echo
echo ">>> Concurrent: --threads ${CONCURRENT_THREADS}"
CONCURRENT_JSON=$(run_one "$CONCURRENT_THREADS" "threads${CONCURRENT_THREADS}")

get_json_number() {
  # Grabs the first match of "key": <number> (space after the colon is
  # optional -- stats.cpp isn't consistent about it) in the file. Relies on
  # our own fixed serialization order (overall block appears before
  # get/set), which is fine since we control the JSON writer.
  grep -o "\"$2\": *[0-9.]*" "$1" | head -1 | cut -d: -f2
}

bt=$(get_json_number "$BASELINE_JSON" "throughput_rps")
ct=$(get_json_number "$CONCURRENT_JSON" "throughput_rps")
bp50=$(get_json_number "$BASELINE_JSON" "p50")
cp50=$(get_json_number "$CONCURRENT_JSON" "p50")
bp95=$(get_json_number "$BASELINE_JSON" "p95")
cp95=$(get_json_number "$CONCURRENT_JSON" "p95")
bp99=$(get_json_number "$BASELINE_JSON" "p99")
cp99=$(get_json_number "$CONCURRENT_JSON" "p99")

echo
echo "=== Comparison: --threads 1 vs --threads ${CONCURRENT_THREADS} ==="
awk -v bt="$bt" -v ct="$ct" \
    -v bp50="$bp50" -v cp50="$cp50" \
    -v bp95="$bp95" -v cp95="$cp95" \
    -v bp99="$bp99" -v cp99="$cp99" \
'BEGIN {
  printf "throughput:   %.0f req/s -> %.0f req/s  (%.2fx)\n", bt, ct, ct/bt
  printf "p50 latency:  %.3f ms -> %.3f ms  (%+.1f%%)\n", bp50, cp50, (cp50-bp50)/bp50*100
  printf "p95 latency:  %.3f ms -> %.3f ms  (%+.1f%%)\n", bp95, cp95, (cp95-bp95)/bp95*100
  printf "p99 latency:  %.3f ms -> %.3f ms  (%+.1f%%)\n", bp99, cp99, (cp99-bp99)/bp99*100
}'

echo
echo "Full results:"
echo "  $BASELINE_JSON"
echo "  $CONCURRENT_JSON"
