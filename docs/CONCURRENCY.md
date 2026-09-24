# Concurrency Model

## 1. Threads

- One event-loop thread owns all socket I/O and connections.
- A fixed-size pool executes commands.
- The TTL implementation may use one maintenance thread if active expiration cannot be integrated
  safely into the event loop.

Prefer `std::jthread` and `std::stop_token` for managed worker lifetimes.

## 2. Storage Locking

Each key maps to one shard. Single-key commands acquire only that shard's lock:

- read path: shared lock when possible
- write/delete/expiration path: exclusive lock

Never hold a shard lock while blocking on a queue, performing socket I/O, logging a large payload,
or sleeping. Version 1 commands touch one key, so code must not hold two shard locks at once.

## 3. Queues

Work and completion queues are bounded and support close/stop behavior. Waiting producers and
consumers must wake during shutdown. Queue methods define whether an item was accepted, rejected
because the queue was closed, or interrupted by stop.

## 4. Connection Safety

File-descriptor numbers can be reused. A work item therefore carries:

- connection ID
- connection generation
- per-connection request sequence
- owned command data

The event loop discards a completion if its connection generation is no longer current. Workers
never retain pointers or references to connection objects.

## 5. Response Ordering

Within one connection, responses must be written in request order even when workers complete out of
order. No ordering guarantee exists across different connections.

## 6. TTL Races

Each stored entry has a generation that changes on replacement. An expiration record may delete a
key only if the stored generation and deadline still match. This prevents an old TTL record from
deleting a newly written value.

## 7. Shutdown Order

1. A signal handler performs only async-signal-safe notification, such as writing an `eventfd`.
2. The event loop stops accepting connections and disables new reads.
3. The work queue is closed according to the documented drain policy.
4. Workers finish accepted work or observe stop and exit.
5. Remaining completions are processed or explicitly discarded.
6. Client sockets, listener, `epoll`, and wakeup descriptors are closed by their owner.
7. All threads are joined before process exit.

Do not call logging libraries, allocate memory, lock mutexes, or perform complex cleanup directly
from a POSIX signal handler.

## 8. Required Concurrency Tests

- many readers of the same key
- readers and writers of one key
- independent writes across shards
- replacement racing with expiration
- deletion racing with `GET` and `TTL`
- queue close while producers and consumers wait
- disconnect while requests are in flight
- shutdown while clients are active
- high-volume randomized operations with invariant checking

