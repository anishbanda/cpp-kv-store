# Product Specification

## 1. Goal

Build a high-performance, multithreaded, in-memory key-value server in modern C++ that
demonstrates Linux networking, concurrency, synchronization, memory safety, testing, and
performance engineering.

The first release is intentionally a focused Redis-compatible subset. It is not intended to
replace Redis or reproduce every Redis feature.

## 2. Platform and Toolchain

- Language: C++20
- Runtime platform: Linux
- Build: CMake and Ninja
- Network I/O: nonblocking TCP sockets and `epoll`
- Concurrency: one event loop, a fixed-size worker pool, and sharded storage locks
- Test framework: GoogleTest
- Default port: `6380`
- Wire protocol: subset of RESP2 described in `docs/PROTOCOL.md`

## 3. Version 1 Commands

| Command | Request | Required response semantics |
| --- | --- | --- |
| `PING` | `PING` | Simple string `PONG` |
| `SET` | `SET key value` | Store/replace value, clear old TTL, return `OK` |
| `GET` | `GET key` | Bulk string value or null bulk string |
| `DEL` | `DEL key` | Integer `1` if removed, otherwise `0` |
| `EXISTS` | `EXISTS key` | Integer `1` if live, otherwise `0` |
| `EXPIRE` | `EXPIRE key seconds` | Integer `1` if TTL set, otherwise `0` |
| `TTL` | `TTL key` | Remaining whole seconds, `-1` no TTL, or `-2` missing |

Command names are case-insensitive. Version 1 supports exactly one key per command where a key
is accepted. Extra or missing arguments are protocol errors and must not crash the server.

## 4. Storage Semantics

- Keys and values are binary-safe byte strings when received through RESP bulk strings.
- Maximum key size: 512 bytes.
- Maximum value size: 1 MiB.
- The store contains 32 shards by default; the shard is `hash(key) % shard_count`.
- `SET` atomically replaces an existing value and removes any existing expiration.
- An expired key behaves exactly like a missing key for all commands.
- TTL uses a monotonic clock internally so wall-clock changes do not extend or shorten leases.
- `TTL` rounds down to remaining whole seconds, matching the documented project behavior.
- Version 1 has no persistence; all data is lost when the process exits.

## 5. Networking Requirements

- Bind to a configurable address and port; default to `0.0.0.0:6380`.
- Support at least 1,000 simultaneous connections in a configured Linux environment.
- Use nonblocking listener and client sockets registered with `epoll`.
- Correctly handle partial reads, partial writes, and commands split across multiple TCP packets.
- Correctly handle multiple pipelined commands in one read.
- Preserve response order within each client connection.
- Treat `EAGAIN`/`EWOULDBLOCK` as normal nonblocking conditions.
- Handle peer disconnects, resets, malformed input, and oversized buffers without crashing.
- Cap each connection's unread input buffer at 2 MiB.
- Ignore `SIGPIPE` or use a send mode that prevents it from terminating the process.

## 6. Concurrency Requirements

- The event-loop thread owns file descriptors, connection objects, and all socket I/O.
- Worker threads execute parsed commands but never call `send`, `recv`, `close`, or `epoll_ctl`.
- Work and completion queues are bounded so an overloaded client cannot create unbounded memory
  growth.
- Operations on unrelated storage shards may proceed concurrently.
- A single command touching one key acquires at most one shard lock.
- Graceful shutdown follows `docs/CONCURRENCY.md` and must not leave joinable threads.
- The test suite must pass under ThreadSanitizer with no known data races.

## 7. Expiration Requirements

- Lazy expiration: storage operations remove a key when they discover it has expired.
- Active expiration: a background mechanism periodically removes expired keys.
- Updating or deleting a key must not allow a stale expiration record to remove a newer value.
- The expiration mechanism must avoid holding a global lock while scanning the entire database.

## 8. Configuration and Observability

Version 1 must accept command-line options for:

- bind address
- port
- worker count
- shard count (power of two)
- queue capacity
- log level

Required logging events include startup configuration, bind/listen failure, malformed requests,
resource-limit failures, shutdown start, and shutdown completion. Logs must not include complete
values or other potentially large client payloads.

## 9. Out of Scope for Version 1

- Persistence, append-only files, snapshots, and crash recovery
- Replication, clustering, consensus, and distributed locks
- Transactions, Lua scripting, pub/sub, streams, and authentication
- TLS implementation
- LRU/LFU eviction and hard memory quotas
- Full Redis command or RESP3 compatibility

Persistence and eviction are candidates for Version 2 only after Version 1 is correct and measured.

## 10. Acceptance Criteria

### Functional

- All seven commands match the documented responses.
- Expired keys cannot be retrieved and report the correct TTL state.
- `SET` replacement clears the previous TTL.
- Malformed and oversized requests receive an error or safe disconnect.

### Networking

- Partial frames and pipelined frames are covered by automated tests.
- Multiple commands per persistent connection work correctly.
- A scripted test creates 1,000 concurrent client connections without server failure.
- Shutdown stops accepting work, drains or cancels according to policy, and exits cleanly.

### Quality

- Debug and release builds succeed with all warnings treated as errors.
- Unit, protocol, integration, and concurrency tests pass.
- ASan/UBSan report no project errors on the complete test suite.
- TSan reports no known project data races on concurrency tests.
- Public interfaces and non-obvious invariants are documented.

### Performance Reporting

- The benchmark reports requests/second plus p50, p95, and p99 latency.
- Workloads and environment are disclosed as specified in `benchmarks/README.md`.
- Results are generated from actual runs and are never hard-coded or estimated.

