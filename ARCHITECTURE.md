# Architecture

## 1. System Overview

```text
TCP clients
    |
    v
TCP listener -> epoll event loop -> per-connection RESP parser
                                      |
                                      v
                              bounded work queue
                                      |
                               worker thread pool
                                      |
                 +--------------------+-------------------+
                 v                                        v
        command dispatcher                    sharded key-value store
                                                          |
                                                   TTL metadata
                                      |
                              bounded result queue
                                      |
                                      v
                              epoll event loop -> clients
```

The design separates socket ownership from command execution. This keeps connection lifecycle and
response ordering under one owner while allowing storage work on different shards to run in
parallel.

## 2. Components

### TCP listener

- Creates a nonblocking listening socket.
- Sets reusable-address options.
- Accepts connections until `accept4` returns `EAGAIN`.
- Applies connection limits and registers accepted descriptors with `epoll`.

### Event loop

- Runs on one dedicated thread, initially the main thread.
- Exclusively owns the `epoll` descriptor, client descriptors, and connection objects.
- Reads available bytes into bounded per-connection buffers.
- Invokes the incremental RESP parser until no complete frame remains.
- Assigns a monotonically increasing sequence number to each request per connection.
- Enqueues parsed commands for workers.
- Receives completed responses, restores per-connection order, and performs nonblocking writes.
- Handles connection timeouts, disconnects, backpressure, and shutdown signaling.

### RESP parser

- Is incremental: incomplete input returns `NeedMoreData`, not an error.
- Produces an owned command object whose data is independent of the connection input buffer.
- Rejects invalid lengths, excessive nesting, unsupported types, oversized keys/values, and integer
  overflow.
- Does not execute commands or access storage.

### Worker pool

- Uses a fixed number of `std::jthread` workers.
- Pulls work from a bounded blocking queue.
- Dispatches validated commands to storage.
- Produces serialized response buffers and posts them to a bounded completion queue.
- Never touches client sockets or connection objects directly.

### Storage engine

- Owns a configurable vector of independent shards.
- Each shard contains an `unordered_map<string, Entry>` guarded by `std::shared_mutex`.
- Reads may use shared access when expiration cannot require mutation; expired-key cleanup upgrades
  by releasing and reacquiring an exclusive lock, then revalidates the entry.
- Writes acquire only the target shard's exclusive lock.
- `Entry` stores the value, optional monotonic expiration time, and a generation number.

### TTL manager

- Combines lazy expiration with bounded active cleanup.
- May keep per-shard expiration heaps or another documented structure.
- Every expiration record includes a generation token. Cleanup deletes only if key, deadline, and
  generation still match the current entry.
- Processes a bounded amount of expiration work per cycle to avoid long pauses.

## 3. Data Ownership

| Resource | Sole owner or synchronization |
| --- | --- |
| Listening and client file descriptors | Event-loop thread |
| `epoll` registration | Event-loop thread |
| Connection buffers and parser state | Event-loop thread |
| Parsed work item | Moved from event loop to one worker |
| Serialized completion | Moved from worker to event loop |
| Storage shard map | Corresponding shard `shared_mutex` |
| Queue state | Queue's mutex and condition variables |
| Shutdown request | `std::stop_token` plus event-loop wakeup descriptor |

Workers refer to a connection only through an opaque connection ID and sequence number. A stale
completion for a disconnected or reused descriptor is discarded by connection-generation checks.

## 4. Backpressure

Both work and completion queues are bounded. If the work queue is full, the event loop temporarily
disables read interest for affected connections or returns a documented busy error. Each connection
also has maximum input and output buffer sizes. Slow clients cannot consume unbounded memory.

Exact watermarks belong in configuration and tests. They must be documented when implemented.

## 5. Response Ordering

Workers may finish commands out of order. Each connection assigns request sequence numbers and the
event loop retains out-of-order completions in a small ordered structure. It writes only the next
expected sequence. Limits on in-flight requests bound this reorder buffer.

## 6. Error Model

- Expected client errors become RESP error replies when the connection can remain synchronized.
- Framing errors that prevent resynchronization close the connection after an optional error reply.
- Resource exhaustion rejects work safely and is logged at an appropriate level.
- Internal invariant failures are tested aggressively; production code returns a controlled error
  or initiates graceful shutdown rather than continuing with corrupt state.

## 7. Portability Boundary

Core storage, protocol, and concurrency primitives should not depend on `epoll`. Linux-specific
networking code belongs under a networking boundary so a future `kqueue` backend is possible, but
Version 1 implements only Linux `epoll`.

## 8. Future Extensions

Version 2 may add append-only persistence, snapshots, and memory eviction. Replication or clustering
requires a separate design review and must not be layered onto Version 1 without revisiting failure
semantics and consistency guarantees.

