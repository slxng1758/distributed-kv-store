# distributed-kv-store

A distributed, in-memory key-value store built from scratch in C++ to
understand what's actually happening underneath tools like Redis --
networking, concurrency, sharding, replication, and failure recovery.

This is a fundamentals project, not tied to a specific application.

## Status: Phase 1 -- single-node store + benchmark harness

- `kvserver` -- single-threaded, `poll()`-based TCP server (GET/SET/DELETE)
- `kvclient` -- one-shot CLI and interactive REPL for manual testing
- `kvbench` -- multithreaded load generator with p50/p95/p99 latency and
  throughput reporting

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire protocol and
[docs/BENCHMARK.md](docs/BENCHMARK.md) for the benchmark methodology.

## Build & run

```
brew install cmake   # if not already installed
cmake -S . -B build && cmake --build build -j

./build/kvserver --port 6380          # in one terminal
./build/kvbench                        # standard workload, in another

ctest --test-dir build                 # unit tests
./tests/smoke_test.sh                  # end-to-end correctness check
./scripts/run_benchmark.sh             # build -> test -> benchmark, end to end
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
