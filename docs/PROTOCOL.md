# Wire Protocol

A memcached-ASCII-inspired hybrid: newline-delimited command lines with a
length-prefixed, binary-safe value payload. This is a deliberate middle
ground between pure inline text (Redis's old inline protocol -- simple, but
values can't safely contain spaces or newlines) and a fully binary protocol
like RESP (binary-safe and fast, but not typeable/debuggable by hand). Value
bytes are read by exact count rather than scanned for a terminator, so a
value may contain any bytes at all, including embedded newlines, with no
escaping needed.

## Framing

`\n` (LF) terminates every command line (a deliberate simplification versus
`\r\n`-based protocols; a trailing `\r` is tolerated and stripped for
compatibility with tools that send CRLF, but is not required). Everything
after a `SET`/`VALUE` length field is read as an exact byte count, followed
by exactly one more `\n`.

## Grammar

```
request      := get-req | set-req | delete-req | ping-req
get-req      := "GET" SP key LF
set-req      := "SET" SP key SP length LF value-bytes LF
delete-req   := "DELETE" SP key LF
ping-req     := "PING" LF

key          := 1*250(VCHAR)        ; no spaces or control chars, max 250 bytes
length       := 1*10(DIGIT)         ; decimal, 0 <= length <= 1048576 (1 MiB)
value-bytes  := length OCTET        ; exactly `length` raw bytes, any value

SP := 0x20   LF := 0x0A

get-response    := ("VALUE" SP length LF value-bytes LF) | "NOT_FOUND" LF | error
set-response     := "OK" LF | error
delete-response  := "DELETED" LF | "NOT_FOUND" LF | error
ping-response    := "PONG" LF
error            := "ERROR" SP message LF
```

`PING` carries no key or value -- it's a pure liveness check (Phase 4's
router failure detector uses it) and never touches the store.

## Bounds

`kMaxKeyLen = 250`, `kMaxValueLen = 1 MiB`. These exist so a malformed or
adversarial length field can't make the server block forever trying to read
an absurd byte count -- a basic DoS-safety consideration, not a complete
one (see Interview Notes below).

## Connections

Connections are **persistent and multi-request**: a client opens one TCP
connection and issues many requests over it, the same way a real Redis
client does. This is why the server is built around a `poll()` event loop
rather than a one-request-per-connection model, and why the benchmark
client opens 50 long-lived connections instead of one connection per
request.

## Examples

```
> SET foo 3
> bar
< OK

> GET foo
< VALUE 3
< bar

> GET missing
< NOT_FOUND

> DELETE foo
< DELETED

> SET foo 999999999999
< ERROR malformed SET request
```

## Interview notes -- tradeoffs to be able to explain

- **Text command line + binary-safe value, not full binary/RESP.** Costs
  more per-request parsing overhead than a tight binary format; buys
  hand-debuggability (a human can type `GET foo\n` into a raw socket and
  read the reply). The benchmark's own numbers show this overhead as data,
  not as something hidden.
- **No TLS, no auth, no per-connection rate limiting.** The key/value size
  caps are the only DoS mitigation. Fine for a closed, local, educational
  benchmarking environment; a real production gap, worth naming unprompted.
- **`\n`-only framing, not `\r\n`.** Simpler grammar; a real interop
  protocol would likely need to accept both, which this does tolerate on
  the receiving side without requiring it.
