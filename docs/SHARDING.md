# Phase 3: Sharding

## Architecture

```
Client -> kvrouter (consistent hash ring) -> kvserver node 1
                                           -> kvserver node 2
                                           -> kvserver node 3
```

`kvrouter` speaks the **exact same client-facing wire protocol** as
`kvserver` (see [docs/PROTOCOL.md](PROTOCOL.md)) -- it's a drop-in
replacement from a client's point of view. That's why `kvclient` and
`kvbench` needed zero changes for this phase: point them at the router's
port instead of a node's port and everything just works, including
`tests/smoke_test.sh` (now with an `SMOKE_TEST_EXTERNAL_SERVER=1` toggle so
it can validate an already-running router instead of always spawning its
own bare `kvserver`).

Backend `kvserver` nodes are **unmodified Phase 1/2 binaries** -- they have
zero sharding awareness. All routing logic lives in `kvrouter`, built once
at startup from `--nodes host:port,...` and never changed at runtime in
Phase 3 (no dynamic membership yet -- that's Phase 4's job, alongside
failure detection and failover).

`kvrouter` is thread-per-connection: each client connection gets its own
thread doing blocking reads, a `ConsistentHashRing::get_node()` lookup, a
forward to the owning backend (over a persistent, per-thread-cached
connection), and a relay of the response back to the client. This is a
different tradeoff from Phase 1/2's `poll()`-based multiplexing -- that
lesson was already established, and Phase 3's actual teaching focus is the
hashing/sharding logic, not another I/O model. See
[docs/CONCURRENCY.md](CONCURRENCY.md) for the poll()-based approach.

## Consistent hashing

`src/common/hash_ring.hpp` implements a ring with virtual nodes: each
physical node is placed at 150 (configurable via `--vnodes`) positions
around a 64-bit ring, and a key is routed to the node owning the first
position at or after `hash(key)`, wrapping around if none is found.

**Why this over `hash(key) % num_nodes`**: modulo hashing ties a key's
owner to the *current node count*. Add or remove one node and the divisor
changes for every key, so nearly all of them remap -- catastrophic for a
cache or store, since it means a near-total cache-miss storm or a
near-total data migration every time the cluster resizes. Consistent
hashing ties a key's owner to its position relative to its *neighbors on
the ring*, which doesn't change when a distant node is added or removed --
so only the fraction of keys actually owned by the changed node moves.

`tests/unit/hash_ring_test.cpp::test_removing_a_node_only_moves_its_own_keys`
proves this directly: build a ring, record every key's owner, remove one
node, and assert that no key whose prior owner *wasn't* the removed node
changed owners. This is the single property that justifies the whole
technique.

### A hash-quality bug worth knowing about

The first implementation used raw FNV-1a for ring positions and produced a
wildly unbalanced ring (one node got 19% of keys, expected ~33%,
`test_distribution_balance` failed with 42% deviation). Cause, confirmed
by printing raw hashes: FNV-1a has weak avalanche for strings sharing a
long common prefix and differing only in a late byte -- exactly what
`"nodeA#0"` vs `"nodeA#1"` vs ... `"nodeA#149"` looks like. The top byte of
`fnv1a_hash("nodeA#0")` through `fnv1a_hash("nodeA#7")` was identical,
which is disastrous when ring position ordering (`std::map<uint64_t,...>`)
is dominated by high-order bits -- all 150 virtual nodes for a given
physical node clustered into a narrow arc instead of spreading around the
ring. Fixed by running the FNV-1a output through MurmurHash3's `fmix64`
finalizer before using it as a ring position (`src/common/hash_ring.cpp`).
Worth knowing for an interview: a hash being "fine" for a hash table
doesn't mean it's fine for a *ring* -- the failure mode is different and
easy to miss without actually printing the numbers and checking balance.

## Benchmark results

Run via `./scripts/run_sharding_comparison.sh`, which also runs
`kvhashreport` for the distribution numbers below.

### Key-distribution balance -- the clean, reproducible win

`kvhashreport` deterministically computes both metrics with no servers
required, using the same 1,000-key default keyspace as `kvbench`:

**Static balance across 3 nodes** (1,000 keys) -- both schemes are already
reasonably balanced at this scale:

| scheme | max deviation from ideal |
|---|---|
| modulo hashing | 7.9% |
| consistent hashing (150 vnodes/node) | 4.9% |

**Remap fraction when scaling 3 -> 4 nodes** -- this is where the two
schemes actually diverge, and it's the metric that matters operationally:

| scheme | % of keys that change node |
|---|---|
| modulo hashing | 75.1% |
| consistent hashing | 27.6% (theoretical ideal: ~25%) |

This is a real, deterministic, always-reproducible result -- it doesn't
depend on network conditions, machine load, or anything about the local
dev box, which is exactly why it's the more defensible of Phase 3's two
headline numbers.

### Throughput: 1 node vs 3 nodes -- an honest, inconclusive result

The standard workload run through `kvrouter` at `--nodes` pointing to 1
node vs all 3 (same router binary both times, isolating node count as the
variable):

| config | throughput | vs. 1-node |
|---|---|---|
| 1 node (10K requests) | 76,430 req/s | baseline |
| 3 nodes (10K requests) | 67,201 req/s | 0.88x |

That looked like a clear loss, so -- same diligence as Phase 2's
concurrency result -- I checked whether it was a fixed-cost artifact (the
3-node config opens up to 3x as many backend connections, up to 150 vs 50,
and that one-time connection-setup cost is a bigger fraction of a 10K-
request run) rather than a sustained effect:

| config | requests | concurrency | throughput | vs. 1-node |
|---|---|---|---|---|
| 1 node | 200,000 | 50 | 76,426 req/s | baseline |
| 3 nodes | 200,000 | 50 | 80,272 req/s | 1.05x |
| 1 node | 200,000 | 150 | 76,582 req/s | baseline |
| 3 nodes | 200,000 | 150 | 72,271 req/s | 0.94x |

Three data points -- 0.88x, 1.05x, 0.94x -- bouncing around 1.0x with no
consistent direction. That's a noisy, inconclusive signal, not a reliable
scaling win, and I'm reporting it as such rather than cherry-picking the
1.05x run.

**Why it's inconclusive here, honestly**: the router, all 3 backend node
processes, and the benchmark client are all running on **one machine's 15
shared cores**. "Sharding" in this setup doesn't add independent hardware
capacity -- it adds an extra network hop (client -> router -> node) and
more concurrent connections, while every process is still drawing from the
same finite pool of cores the 1-node config was already using. This
mirrors Phase 2's finding almost exactly: horizontal scaling techniques
(more threads, more nodes) measured on a single co-located machine mostly
expose coordination overhead, not the capacity increase they're meant to
provide. To actually see sharding's throughput benefit, the nodes (and
ideally the router and the client) need to be on genuinely separate
machines, or at minimum pinned to disjoint CPU cores, so that adding a
node adds real, independent compute rather than more contention for the
same 15 cores.

## Interview-readiness notes

1. **Why consistent hashing, concretely**: the 75.1% vs 27.6% remap-
   fraction number *is* the answer -- modulo hashing makes cluster resizes
   catastrophic for cache/data locality; consistent hashing bounds the
   damage to roughly `1/(new node count)`.
2. **The hash-quality bug**: a hash function can be adequate for a hash
   table and still be wrong for a ring, because ring position ordering
   depends on avalanche across the *whole* output, not just enough entropy
   to avoid table collisions. Caught by actually checking the balance
   numbers, not by assumption.
3. **Nodes are unmodified**: all sharding logic is in the router; backend
   nodes have zero cluster awareness. This is a real, common production
   pattern (e.g. Twemproxy/mcrouter in front of vanilla
   memcached/Redis instances).
4. **What's NOT here yet**: no dynamic node membership, no failure
   detection, no automatic failover -- a node going down currently
   surfaces as a clean `ERROR` to the client (Router::handle_client drops
   the broken cached connection and reports the failure) rather than being
   retried or rerouted. That's explicitly Phase 4's job.
5. **The throughput result is honestly inconclusive, and that's the more
   interesting answer**: a naive "3 nodes should be ~3x faster" claim
   would not survive the follow-up question "did you test that?" The real
   answer -- co-located processes on one machine don't add independent
   capacity, so the benefit doesn't show up reliably here -- demonstrates
   understanding the *mechanism* of horizontal scaling, not just the
   vocabulary.
