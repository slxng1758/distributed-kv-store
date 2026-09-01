# Phase 4: Replication + Failover

## Architecture

`kvrouter` now replicates each key to `--replicas` (default 2) distinct
nodes instead of routing to exactly one. The replica set for a key is its
**preference list**: `ConsistentHashRing::get_nodes(key, replication_factor)`
-- the primary (same node `get_node()` would pick) plus the next
`replication_factor - 1` distinct physical nodes walking clockwise around
the ring.

## Consistency model

- **WRITE (SET/DELETE)**: sent to every replica in the preference list that
  the failure detector currently considers alive. Succeeds if at least one
  replica acknowledges; fails with `ERROR no live replica for key` only if
  every replica in the list is down.
- **READ (GET)**: tried against replicas in preference-list order (primary
  first); returns the first successful response.

This is deliberately simple: no quorum (`W`/`R` aren't configurable, it's
"all live replicas" for writes and "first live replica" for reads), no
version vectors, no read-repair, no anti-entropy. In the no-failure steady
state this is effectively strongly consistent -- every write reaches every
live replica before the client sees `OK`, so any subsequent read from any
live replica sees the latest value.

**The gap, stated plainly**: a replica that was down *during* a write never
gets that write. When it comes back up (the heartbeat notices and marks it
alive again), it silently rejoins the read/write rotation still missing
whatever changed while it was gone -- there is no catch-up mechanism. A
read that happens to land on that replica can return stale data or
`NOT_FOUND` for a key that exists on its healthy peers. Real systems close
this gap with hinted handoff (a surviving replica temporarily buffers
writes meant for the down node and replays them on recovery) or
anti-entropy / read-repair (background reconciliation, e.g. via Merkle
trees). Neither exists here -- an explicit, named scope boundary for this
phase, not an oversight.

## Failure detection

Two complementary paths, both in `src/router/failure_detector.*`:

1. **Heartbeat** (200ms interval, 400ms timeout via `SO_RCVTIMEO`/
   `SO_SNDTIMEO`): a background thread does a fresh connect-PING-close to
   every node each cycle (simpler to get right than managing a persistent,
   reconnecting heartbeat socket, and a TCP handshake on localhost at this
   interval is cheap). This is what notices a node **coming back up**, and
   what proactively keeps routing away from a node nobody has tried
   recently.
2. **Reactive marking**: a request-handling thread calls `mark_down()`
   the instant a forward to a node fails -- it doesn't wait for the next
   heartbeat tick. This is what makes failover fast: detection happens at
   the moment of actual failure, not on a fixed cadence.

`PING`/`PONG` is a new protocol command (see `docs/PROTOCOL.md`) that
touches nothing in `KVStore` -- pure liveness check.

## Benchmark: killing a node mid-run

`kvfailover` (`src/tools/failover_bench.cpp`) generates continuous load
against the router from N persistent connections, and at a precise,
in-process-timed moment sends `SIGKILL` directly to a node's PID (a raw
`kill()` syscall from inside the same process, not a separate shell
command -- avoids the synchronization slop of coordinating a kill through
bash `sleep`). It then classifies every request's outcome as before/after
the kill and reports the recovery window. Run via
`./scripts/run_failover_benchmark.sh`.

### Result: --replicas 2 (the real design)

Representative run: 4-second load, 20 connections, kill at t=1500ms.

```
requests before kill: 106,638 (errors: 0, as expected)
requests after kill:  204,519 (errors: 0)
recovery time: 0ms -- no request failed after the kill at all
```

**Zero client-visible errors**, reproduced across many independent runs
(different node killed each time). This isn't a measurement fluke -- see
below -- and it's the mechanism, not magic: `route_read`'s loop over the
preference list retries the *next* replica within the same request the
instant the primary's connection fails, so a request that happens to hit
the dying node transparently falls back to the survivor before the client
ever sees a response. With a 2-node preference list and only one node
killed, there is always a survivor.

### Contrast: --replicas 1 (no redundancy)

Same scenario, same kill, only the replication factor changed:

```
requests before kill: 129,648 (errors: 0, as expected)
requests after kill:  191,687 (errors: 62,200)
recovery time: NOT BOUNDED -- errors still occurring at the end of the run
```

With no fallback replica, every request for a key owned by the killed node
fails, permanently -- roughly 1/3 of all traffic (the killed node's share
of the 3-node keyspace), continuing at a steady ~30% error rate for as
long as the node stays down. `kvfailover` detects this itself: if errors
are still happening in the final 200ms of the run, it refuses to report a
"recovery time" number and says so explicitly, rather than printing
something that looks like a fast recovery but is actually just an artifact
of the test ending. This contrast is the proof that the `--replicas 2`
result above is a genuine consequence of replication, not an artifact of
how the benchmark works.

### The debugging story (worth keeping, not editing out)

The first version of this benchmark reported 2-5 stray errors *before* the
kill timestamp on `--replicas 1` runs -- consistently, reproducibly, and
initially unexplained (a run with `--replicas 2` never showed this). The
instinct to distrust a too-clean "0ms, 0 errors" result and go dig into a
smaller, uglier one paid off:

- Doubling the heartbeat timeout (150ms -> 400ms) didn't change anything,
  ruling out "heartbeat false-positives under CPU load" as the cause.
- Temporary diagnostic logging in the router's forwarding failure path
  showed every one of these "before kill" failures was a real
  `connection closed while waiting for response` against the node that
  *was about to be* killed -- i.e., genuinely caused by the kill, just
  timestamped wrong.
- Root cause: `kvfailover` classified each request as before/after the
  kill using its **start** timestamp. A request that started a few
  milliseconds before the kill but whose in-flight read was interrupted by
  it only learns its outcome *after* the kill -- classifying by start time
  attributes a kill-caused failure to "before the kill," which is wrong.
  With `--replicas 2`, this never surfaced as a bug because the same
  in-flight retry-to-the-next-replica logic that produces the 0-error
  headline result also absorbed these straddling failures silently.
- Fix: classify by **completion** timestamp instead (`end_ms`, not
  `start_ms`) -- a request only "counts" against a before/after bucket once
  its outcome is actually known. After the fix, `--replicas 1` runs show
  zero errors before the kill, as they should.

This is included here rather than cleaned out of the history because it's
a more convincing demonstration of the 0ms/0-error result's legitimacy
than the result alone would be: it was checked, not assumed.

## Interview-readiness notes

1. **Consistency model, precisely**: not strongly consistent under
   partition (a replica down during a write silently misses it forever,
   no repair), not quorum-based, not really clean AP-vs-CP CAP-theorem
   language either -- "best-effort N-way replication, no anti-entropy" is
   the honest description, and being able to say that instead of reaching
   for CAP jargon that doesn't quite fit is the stronger answer.
2. **Why failover is fast here**: not because failure detection is
   instant (the heartbeat is 200ms), but because failover doesn't *depend*
   on the heartbeat at all in the common case -- reactive marking plus a
   same-request retry to the next replica means the client never sees the
   failure as long as one replica survives.
3. **What breaks this**: killing 2 of 3 nodes (more than `replicas - 1`)
   would exhaust every replica for some keys and reproduce the
   `--replicas 1`-style persistent-error pattern even at `--replicas 2` --
   worth being ready to say plainly rather than implying replication
   protects against everything.
4. **The stale-replica-on-rejoin gap**: a killed node that restarts comes
   back with an empty (or, more generally, however-stale) `KVStore` and
   rejoins the rotation immediately once heartbeats succeed again, with no
   catch-up. This is the single most important "what would you add next"
   answer for this phase.
5. **Debugging discipline**: the 0ms/0-error result survived scrutiny
   specifically because a smaller, uglier, inconsistent result (2-5 stray
   errors) was investigated instead of dismissed as noise -- a real example
   of the difference between a number that looks good and a number that's
   been checked.
