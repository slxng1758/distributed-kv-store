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
- All reactors share one `KVStore`. It started out (see "Original result"
  below) as a single `std::shared_mutex` over the whole map; it's now
  sharded into 16 independent partitions, each with its own
  `std::shared_mutex` (see "Optimization: lock striping"). `--threads 1`
  runs this same binary through the same code path and is functionally
  equivalent to Phase 1's single loop either way.

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

## Original result: single global lock

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

## Optimization: lock striping

The obvious fix for "every thread contends on one global lock regardless
of which key it touches" is to stop using one lock. `KVStore` (see
`src/server/kv_store.hpp`) now partitions its keyspace into 16 independent
shards, each with its own map and its own `std::shared_mutex`, keyed by
`fnv1a_hash(key) % 16` (reusing the same well-mixed hash Phase 3's ring
already uses). Two threads touching different shards now run fully in
parallel with zero shared state -- the same principle behind Java's
`ConcurrentHashMap` and Redis Cluster's per-slot ownership. `size()` is
tracked via a separate `std::atomic<size_t>` rather than summing all 16
shards under lock, so it stays exact and lock-free.

**Result on the standard workload (10K requests, 64-byte values):
essentially unchanged.** Re-running the exact same 1/2/4/8-thread sweep
from above against the sharded store:

| threads | throughput, global lock (before) | throughput, sharded lock (after) |
|---------|-----------------------------------|-----------------------------------|
| 1       | 192,532 req/s                     | 219,538 req/s                     |
| 2       | 191,021 req/s                     | 207,841 req/s                     |
| 4       | 150,685 req/s                     | 157,316 req/s                     |
| 8       | 145,175 req/s                     | 144,365 req/s                     |

Still a monotonic decline as thread count increases. This is the useful
negative result: if lock contention were the dominant bottleneck, sharding
the lock 16 ways should have fixed it. It didn't, which rules out lock
contention as the primary cause and points at something more fundamental
-- for an operation this cheap (tens of nanoseconds of actual work), the
overhead of cross-thread coordination itself (OS scheduling, waking a
thread via `poll()`, moving data between core caches) exceeds the work
being parallelized, no matter how finely the lock is partitioned. (Ruled
out separately: false sharing between adjacent shards -- `sizeof(Shard)`
is 208 bytes, several cache lines, so adjacent shards' hot fields don't
share a cache line.)

**Result once there's real work per request: a genuine win.** If the
theory above is right -- overhead dominates only because the work is too
small -- then a heavier per-request payload should tip the balance back
toward concurrency actually helping. Reproducible via:

```
CONCURRENT_THREADS=2 KVBENCH_EXTRA_ARGS="--requests 200000 --value-size 65536" \
  ./scripts/run_concurrency_comparison.sh
```

(64 KiB values instead of the standard 64 bytes -- 1,000x more bytes to
copy into/out of the map and over the socket per request.)

| metric      | threads=1 | threads=2 | delta |
|-------------|-----------|-----------|-------|
| throughput  | 77,473 req/s | 92,838 req/s | **1.20x** |
| p50 latency | 0.579 ms  | 0.463 ms  | **-20.1%** |
| p95 latency | 1.052 ms  | 0.893 ms  | **-15.2%** |
| p99 latency | 1.425 ms  | 1.088 ms  | **-23.7%** |

Reproduced across independent runs (1.20x-1.22x). A sweep found 2 threads
is the sweet spot for this payload size on this machine; 4 and 8 threads
give a smaller win than 2, consistent with the same coordination-overhead
cost from the negative result above starting to eat back into the gains
past a certain thread count, rather than scaling linearly forever.

**The honest summary**: this store's concurrency model scales when there's
enough real work per request to amortize thread-coordination overhead, and
doesn't when there isn't -- the standard 64-byte-value benchmark workload
sits on the "doesn't" side of that line, and no amount of lock
optimization changes that, because the lock was never the bottleneck for
that workload in the first place. That's a more complete and more
defensible answer than either "threading always wins" or "threading never
helped, so why bother."

## Interview-readiness notes

1. **Why did lock striping alone not fix the standard-workload result?**
   Because the global lock was never actually the bottleneck for that
   workload -- the diagnostic that proved it: shard the lock 16 ways, keep
   everything else the same, rerun the identical sweep, and the same
   monotonic decline still shows up. That result is what justified moving
   on to testing whether request *size* (not lock granularity) explained
   the ceiling, rather than continuing to tune locking strategy against
   the wrong hypothesis.
2. **Locking strategy**: 16-way sharded `std::shared_mutex`, keyed by
   `fnv1a_hash(key) % 16`; shared lock for reads, exclusive for writes on
   each shard independently. Started as one global lock (a textbook single
   point of contention); striping it is the same principle Phase 3 applies
   across nodes, applied here within one node's data structure.
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
6. **Concurrency benefit depends on payload size, and that's the real
   finding**: 64-byte values never show a throughput win regardless of
   locking strategy; 64 KiB values show a reproducible 1.20x-1.22x win at
   `--threads 2` with every latency percentile improved. Knowing *why*
   (coordination overhead vs. actual work per request) is what makes this
   a systems-fundamentals result instead of a benchmark number picked
   because it looked good.
