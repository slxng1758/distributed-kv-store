# distributed-kv-store

A distributed, in-memory key-value store built from scratch in C++ to
understand what's actually happening underneath tools like Redis --
networking, concurrency, sharding, replication, and failure recovery.

This is a fundamentals project, not tied to a specific application.

## Status: Phase 2 -- concurrency

- `kvserver` -- N `poll()`-based reactor threads (`--threads`, default 4),
  each owning a disjoint set of connections, sharing one lock-protected
  `KVStore`. `--threads 1` reproduces Phase 1's single-threaded behavior.
- `kvclient` -- one-shot CLI and interactive REPL for manual testing
- `kvbench` -- multithreaded load generator with p50/p95/p99 latency and
  throughput reporting

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire protocol,
[docs/BENCHMARK.md](docs/BENCHMARK.md) for the benchmark methodology, and
[docs/CONCURRENCY.md](docs/CONCURRENCY.md) for the Phase 2 architecture,
correctness story, and the (honest, non-obvious) `--threads 1` vs
`--threads N` benchmark result.

## Build & run

```
brew install cmake   # if not already installed
cmake -S . -B build && cmake --build build -j

./build/kvserver --port 6380          # in one terminal
./build/kvbench                        # standard workload, in another

ctest --test-dir build                 # unit tests
./tests/smoke_test.sh                  # end-to-end correctness check
./scripts/run_benchmark.sh             # build -> test -> benchmark, end to end
./scripts/run_concurrency_comparison.sh   # --threads 1 vs --threads N, same binary
```

## Manual testing

```
./build/kvclient --port 6380
> SET foo bar
OK
> GET foo
bar
> DELETE foo
DELETED
```
