# Benchmark Methodology

## The standard workload

`kvbench` with no arguments always runs the same workload, so numbers stay
comparable, unchanged, across every later phase (concurrency, sharding,
replication):

- 10,000 requests
- 50 concurrent, persistent client connections (one TCP connection per
  worker thread, reused for all of that thread's requests -- not one
  connection per request)
- 70% GET / 30% SET
- 1,000-key keyspace, 64-byte values
- seeded (`--seed 42`) for reproducibility

Run it with `./scripts/run_benchmark.sh`, or manually:

```
cmake -S . -B build && cmake --build build -j
./build/kvserver --port 6380 &
./build/kvbench            # standard workload, defaults only
```

## TCP_NODELAY -- read this before trusting any latency number

Both `kvserver` and `kvbench` explicitly set `TCP_NODELAY` on every socket.
Without it, Nagle's algorithm on the sending side interacting with delayed
ACKs on the receiving side can add tens of milliseconds of entirely
artificial latency to small request/response exchanges like these -- and it
will silently show up as inflated p50/p95/p99 numbers with no error or
warning. This is a classic, well-known benchmarking pitfall; if you ever
port this workload to a different language/runtime, verify `TCP_NODELAY` is
still being set before comparing numbers.

## Measurement method

- **Closed-loop, non-pipelined.** Each of the 50 worker threads has exactly
  one request in flight at a time: send, block until the full response is
  parsed, record the latency, send the next. This mirrors realistic client
  behavior. It does **not** measure the server's maximum throughput under
  pipelining -- a named limitation, not an oversight.
- **Client-observed latency, `steady_clock`**, timed around a single
  request/response round trip per sample.
- **Throughput** = `completed_requests / wall_clock_seconds`, measured
  around the whole concurrent run (not summed per-thread, which would
  double-count).
- **Percentiles**: samples are sorted and `index = ceil(p * n) - 1`
  (clamped to `[0, n-1]`) selects the value. Different tools interpolate
  percentiles differently -- this exact formula is what makes numbers
  comparable run-to-run and phase-to-phase, so it must not change later.
- **Client and server share one machine** (localhost). This includes OS
  scheduling and CPU-contention noise from co-locating them in the
  reported numbers -- a documented caveat. Running the client and server on
  separate machines, or pinning each to its own CPU cores, is the natural
  follow-up if more rigor is needed.

## Output artifacts

Every run produces:

- `results/<run-id>.json` -- the stable, cross-phase comparison contract:
  full config (including `git_rev`, so a result can be traced back to the
  exact code that produced it), `throughput_rps`, and `latency_ms` with
  `p50`/`p95`/`p99`/`mean`/`min`/`max`, broken out overall and per GET/SET,
  plus `error_count`.
- `results/<run-id>.csv` (only with `--raw-output`) -- every raw per-request
  latency sample, for later plotting.
- `results/<run-id>.server_rusage.txt` -- peak RSS and CPU time for the
  server process, captured via `/usr/bin/time -l` (macOS/BSD) by
  `run_benchmark.sh`, covering the "CPU/memory usage" ask with no extra
  instrumentation code.

`run_benchmark.sh` always runs `tests/smoke_test.sh` first and aborts if it
fails -- a benchmark run against a broken server produces numbers that look
plausible and are worthless, which is worse than no numbers at all.

## Reusing this workload in later phases

Phase 2 (concurrency): re-run `./build/kvbench` with identical defaults
against the same server binary with threading enabled -- the throughput/p95
delta is the first real, self-generated number.

Phase 3 (sharding) / Phase 4 (replication): point `--host`/`--port` at the
router or a specific node and keep every other flag unchanged, so the
`results/*.json` files across phases 1-4 remain directly diffable.
