#!/usr/bin/env bash
# Phase 3's headline metrics: (1) throughput at 1 node vs 3 nodes, both
# reached through the SAME kvrouter binary so the comparison isolates the
# number-of-nodes variable rather than conflating it with "added a proxy
# hop"; (2) a deterministic key-distribution-balance report (modulo vs
# consistent hashing) via kvhashreport, which needs no running servers at
# all.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
ROUTER_PORT="${ROUTER_PORT:-6380}"
NODE_BASE_PORT="${NODE_BASE_PORT:-7001}"
NUM_NODES=3

echo "=== Building ==="
cmake -S . -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

echo "=== Running unit tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

stop_pid() {
  local pid="$1"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

NODE_PIDS=()
cleanup() {
  for pid in "${NODE_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && stop_pid "$pid"
  done
}
trap cleanup EXIT

wait_ready() {
  local port="$1"
  for _ in $(seq 1 50); do
    if "$BUILD_DIR/kvclient" --port "$port" GET __probe__ >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "server on port $port never came up" >&2
  return 1
}

echo
echo "=== Starting $NUM_NODES kvserver nodes ==="
NODE_ADDRS=()
for i in $(seq 0 $((NUM_NODES - 1))); do
  p=$((NODE_BASE_PORT + i))
  "$BUILD_DIR/kvserver" --port "$p" >"/tmp/kvnode_${p}.log" 2>&1 &
  NODE_PIDS+=("$!")
  NODE_ADDRS+=("127.0.0.1:${p}")
  wait_ready "$p"
done
echo "Nodes: ${NODE_ADDRS[*]}"

mkdir -p results
RUN_ID="$(date +%Y%m%d-%H%M%S)"

ALL_NODES_CSV="$(IFS=,; echo "${NODE_ADDRS[*]}")"
ONE_NODE_CSV="${NODE_ADDRS[0]}"

export BUILD_DIR

echo
echo "=== 3-node sharded config: starting kvrouter ==="
"$BUILD_DIR/kvrouter" --port "$ROUTER_PORT" --nodes "$ALL_NODES_CSV" >/tmp/kvrouter3.log 2>&1 &
ROUTER_PID=$!
wait_ready "$ROUTER_PORT"

echo "=== Smoke test through 3-node router ==="
SMOKE_TEST_PORT="$ROUTER_PORT" SMOKE_TEST_EXTERNAL_SERVER=1 ./tests/smoke_test.sh

echo "=== Benchmark: 3-node sharded ==="
"$BUILD_DIR/kvbench" --port "$ROUTER_PORT" --output "results/${RUN_ID}-shard3.json"

echo "=== Stopping 3-node router ==="
stop_pid "$ROUTER_PID"

echo
echo "=== 1-node config (via the same router binary): starting kvrouter ==="
"$BUILD_DIR/kvrouter" --port "$ROUTER_PORT" --nodes "$ONE_NODE_CSV" >/tmp/kvrouter1.log 2>&1 &
ROUTER_PID=$!
wait_ready "$ROUTER_PORT"

echo "=== Benchmark: 1-node (via router) ==="
"$BUILD_DIR/kvbench" --port "$ROUTER_PORT" --output "results/${RUN_ID}-shard1.json"

echo "=== Stopping 1-node router ==="
stop_pid "$ROUTER_PID"

# NODE_PIDS cleanup (the 3 backend kvserver processes) happens via the
# EXIT trap once this script ends.

echo
echo "=== Key-distribution balance report (modulo vs. consistent hashing) ==="
"$BUILD_DIR/kvhashreport" --output "results/${RUN_ID}-hash-distribution.json"

get_json_number() {
  grep -o "\"$2\": *[0-9.]*" "$1" | head -1 | cut -d: -f2
}

t1=$(get_json_number "results/${RUN_ID}-shard1.json" "throughput_rps")
t3=$(get_json_number "results/${RUN_ID}-shard3.json" "throughput_rps")

echo
echo "=== Comparison: 1 node vs 3 nodes (both via kvrouter) ==="
awk -v t1="$t1" -v t3="$t3" \
    'BEGIN { printf "throughput: %.0f req/s -> %.0f req/s (%.2fx)\n", t1, t3, t3/t1 }'

echo
echo "Results:"
echo "  results/${RUN_ID}-shard1.json"
echo "  results/${RUN_ID}-shard3.json"
echo "  results/${RUN_ID}-hash-distribution.json"
