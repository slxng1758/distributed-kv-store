#!/usr/bin/env bash
# Phase 4's headline metric: kill a node mid-benchmark and measure how
# long requests actually fail afterward. Runs the same scenario twice --
# --replicas 2 (the real design) and --replicas 1 (no redundancy, as a
# contrast case) -- so the comparison demonstrates that any "0ms recovery"
# result is a genuine consequence of replication, not an artifact of the
# test itself.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
ROUTER_PORT="${ROUTER_PORT:-6380}"
NODE_BASE_PORT="${NODE_BASE_PORT:-7001}"
NUM_NODES=3
DURATION_MS="${DURATION_MS:-4000}"
KILL_AT_MS="${KILL_AT_MS:-1500}"

echo "=== Building ==="
cmake -S . -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

echo "=== Running unit tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

stop_pid() {
  kill "$1" 2>/dev/null || true
  wait "$1" 2>/dev/null || true
}

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

mkdir -p results
RUN_ID="$(date +%Y%m%d-%H%M%S)"

# Runs one full trial: start NUM_NODES fresh kvserver processes, start a
# router with the given replication factor, run kvfailover killing the
# first node, tear everything down. Fresh nodes each trial so a kill in
# one trial can't affect the other.
run_trial() {
  local replicas="$1" label="$2"
  local node_pids=()
  local node_addrs=()

  for i in $(seq 0 $((NUM_NODES - 1))); do
    local p=$((NODE_BASE_PORT + i))
    "$BUILD_DIR/kvserver" --port "$p" --threads 2 >"/tmp/kvnode_${label}_${p}.log" 2>&1 &
    node_pids+=("$!")
    node_addrs+=("127.0.0.1:${p}")
    wait_ready "$p"
  done

  local nodes_csv
  nodes_csv="$(IFS=,; echo "${node_addrs[*]}")"

  "$BUILD_DIR/kvrouter" --port "$ROUTER_PORT" --nodes "$nodes_csv" --replicas "$replicas" \
    >"/tmp/kvrouter_${label}.log" 2>&1 &
  local router_pid=$!
  wait_ready "$ROUTER_PORT"

  echo "=== Trial: --replicas $replicas -- killing node at 127.0.0.1:${NODE_BASE_PORT} (pid ${node_pids[0]}) at t=${KILL_AT_MS}ms ==="
  "$BUILD_DIR/kvfailover" --port "$ROUTER_PORT" --kill-pid "${node_pids[0]}" \
    --duration-ms "$DURATION_MS" --kill-at-ms "$KILL_AT_MS" --concurrency 20 \
    --output "results/${RUN_ID}-failover-replicas${replicas}.json"

  stop_pid "$router_pid"
  # node_pids[0] was killed by kvfailover itself (that's the point) --
  # disown it so bash doesn't print an asynchronous "Killed" job-control
  # notice the next time it polls background job state.
  disown "${node_pids[0]}" 2>/dev/null || true
  for pid in "${node_pids[@]:1}"; do
    stop_pid "$pid"
  done
  echo
}

run_trial 2 "rf2"
run_trial 1 "rf1"

echo "=== Results ==="
echo "  results/${RUN_ID}-failover-replicas2.json  (the real design)"
echo "  results/${RUN_ID}-failover-replicas1.json  (no-redundancy contrast case)"
