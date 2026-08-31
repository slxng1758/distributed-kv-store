# Phase 2: Concurrency

## Architecture

Phase 1's single `poll()` event loop is now N independent `WorkerReactor`
instances, each running its own `poll()` loop on its own thread, each
owning a **disjoint** subset of client connections. A single accept thread
(`KvServer::run()`) does a plain blocking `accept()` loop and round-robins
each new connection to one reactor via `enqueue_client()`.

- Within one reactor, Phase 1's model is unchanged: one request in flight
  per connection, interleaved non-blockingly across that reactor's
  connections. No connection is ever fully monopolizing anything.
- Handing a freshly accepted fd from the accept thread to a reactor thread
  needs synchronization: `enqueue_client()` stages the fd behind a mutex
  and wakes the reactor's blocked `poll()` via a self-pipe (macOS has no
  `eventfd`; a pipe is the portable equivalent -- write a byte from the
  accept thread, the reactor's `poll()` sees `poll_fds_[0]` (the pipe's
  read end) become readable and drains it).
- All reactors share one `KVStore`, now internally synchronized with a
  `std::shared_mutex`: `get()`/`size()` take a shared (read) lock, so
  concurrent reads don't block each other; `set()`/`remove()` take an
  exclusive (write) lock. `--threads 1` runs this same binary through the
  same code path and is functionally equivalent to Phase 1's single loop.

## Correctness

`tests/unit/kv_store_test.cpp::test_concurrent_distinct_keys` spins up 8
threads each doing 2,000 SET-then-GET-back cycles on keys unique to that
thread, and asserts zero mismatches and an exact final `size()`. This
passes reliably (`ctest --test-dir build`), which is the guarantee
`std::shared_mutex` actually gives: **individual operations are race-free**.

What it deliberately does **not** claim: a compound "GET, compute, SET"
sequence built from two separate locked calls is not atomic as a whole --
two threads can interleave between their GET and their SET and lose an
update. `KVStore` doesn't expose a compare-and-swap or an atomic INCR, so
building one on top of it yourself would need to hold a lock across both
calls (which the current interface doesn't allow) or add a real atomic
primitive to the store. This is precisely why real stores expose atomic
commands (Redis's `INCR`) instead of leaving read-modify-write to clients.

I tried to verify this under ThreadSanitizer as well (`cmake
-DENABLE_TSAN=ON`), but TSan's runtime itself is broken on this exact
toolchain (Apple clang 15 targeting macOS 26.5.2/arm64) -- even a
`std::thread`-only program with no shared state segfaults before user code
runs. The `ENABLE_TSAN` CMake option is left in place since it should work
on a more standard Linux or older-macOS/Xcode setup; on this machine,
correctness rests on the stress test plus the locking discipline above,
not on sanitizer confirmation.

## Benchmark result: --threads 1 vs --threads N

Run via `./scripts/run_concurrency_comparison.sh`, which builds once and
runs the identical standard workload (10,000 requests, 50 connections,
70/30 GET/SET) against the **same kvserver binary** at `--threads 1` and
`--threads 4`, isolating the concurrency variable.

Representative run on this machine (15 physical cores):

| metric        | threads=1 | threads=4 | delta   |
|---------------|-----------|-----------|---------|
| throughput    | 169,399 req/s | 143,797 req/s | 0.85x (worse) |
| p50 latency   | 0.203 ms  | 0.299 ms  | +46.9% (worse) |
| p95 latency   | 0.610 ms  | 0.492 ms  | -19.4% (better) |
| p99 latency   | 0.642 ms  | 0.617 ms  | -3.9% (better) |

This is **not** a short-run artifact -- re-running with 200,000 requests
instead of 10,000 shows the same pattern, and a sweep confirms it's
monotonic:

| threads | throughput (200K requests) |
|---------|------------------------|
| 1       | 192,532 req/s          |
| 2       | 191,021 req/s          |
| 4       | 150,685 req/s          |
| 8       | 145,175 req/s          |

**More reactor threads make this workload slower, not faster.** That's a
real, reproducible result, not a bug -- and it's a more useful thing to be
able to explain in an interview than a naive throughput win would be:

- The Phase 1 single-reactor loop was *already* servicing all 50
  connections concurrently via non-blocking `poll()` multiplexing -- no
  connection was ever blocked waiting on another. Adding reactor threads
  doesn't unlock previously-serialized work; it adds threads competing for
  a resource that wasn't actually a bottleneck.
- `KVStore` uses one **global** lock for the entire map, not a lock per key
  or per shard. Every GET and SET across every reactor thread contends on
  that same `shared_mutex`, and with only 1,000 distinct keys in the
  default keyspace, multiple threads are very likely touching the same
  region of the map at the same time.
- The actual critical section -- a hash lookup or insert on a handful of
  bytes -- takes on the order of tens of nanoseconds. The synchronization
  overhead (atomic operations on the lock's internal state, cache-line
  invalidation as that state and the map's memory bounce between cores) is
  larger than the work being protected. `std::shared_mutex` in particular
  carries more bookkeeping than a plain `std::mutex` to track reader
  counts, which doesn't pay for itself when critical sections are this
  short.

This is exactly the argument for **Phase 3's sharding**: partitioning the
keyspace across independent nodes (and therefore independent locks, or no
shared lock at all) removes the single point of contention this benchmark
exposes. It's also exactly why real single-threaded stores like Redis
avoid this problem altogether by never sharing mutable state across
threads for the data path in the first place.

## Interview-readiness notes

1. **Why concurrent, if it's not faster here?** Because "add concurrency,
   benchmark it, understand the result" was the assignment -- and the
   honest result teaches the real lesson: naive lock-based sharing doesn't
   scale for tiny, high-contention critical sections. A rehearsed "it got
   faster" answer would be less defensible than being able to explain why
   it didn't, and what would need to change (sharded locks, per-key
   ownership, or avoiding shared mutable state entirely) to fix it.
2. **Locking strategy**: one global `std::shared_mutex` over the whole map;
   shared lock for reads, exclusive for writes. Simple and correct, but a
   textbook example of a single point of contention -- deliberately not
   optimized further here since eliminating it via partitioning is Phase
   3's actual job.
3. **What's NOT atomic**: individual store operations are race-free;
   compound read-modify-write sequences built from separate GET+SET calls
   are not. No CAS/INCR primitive exists yet.
4. **`--threads 1` as baseline**: reproduces Phase 1's exact behavior on
   this same binary, which is what makes the throughput/latency comparison
   above an apples-to-apples measurement of the concurrency change alone.
5. **TSan didn't run here**: the toolchain/OS combination on this dev
   machine has a broken TSan runtime (verified with a trivial repro,
   unrelated to this project's code). Said so plainly rather than claiming
   sanitizer coverage that didn't actually happen.
