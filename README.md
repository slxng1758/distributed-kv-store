# distributed-kv-store

A distributed, in-memory key-value store built from scratch in C++ to
understand what's actually happening underneath tools like Redis --
networking, concurrency, sharding, replication, and failure recovery.

This is a fundamentals project, not tied to a specific application.

## Status: Phase 3 -- sharding

- `kvserver` -- N `poll()`-based reactor threads (`--threads`, default 4),
  each owning a disjoint set of connections, sharing one lock-protected
  `KVStore`. `--threads 1` reproduces Phase 1's single-threaded behavior.
  Zero sharding awareness -- a plain, unmodified node.
- `kvrouter` -- accepts client connections, routes each key to a backend
  node via consistent hashing (`--nodes host:port,...`), forwards, relays
  the response. Speaks the identical client-facing protocol as `kvserver`,
  so `kvclient`/`kvbench` work against it unmodified.
- `kvhashreport` -- deterministic (no servers needed) report comparing
  modulo hashing vs. consistent hashing on key-distribution balance and
  remap fraction when the cluster resizes.
- `kvclient` -- one-shot CLI and interactive REPL for manual testing
- `kvbench` -- multithreaded load generator with p50/p95/p99 latency and
  throughput reporting

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the wire protocol,
[docs/BENCHMARK.md](docs/BENCHMARK.md) for the benchmark methodology,
[docs/CONCURRENCY.md](docs/CONCURRENCY.md) for Phase 2, and
[docs/SHARDING.md](docs/SHARDING.md) for Phase 3's architecture, the
consistent-hashing ring (including a hash-quality bug it exposed), and the
benchmark results -- a clean win on distribution balance, an honest,
inconclusive result on throughput.

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
./scripts/run_sharding_comparison.sh      # 1 node vs 3 nodes via kvrouter, + hash report
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
