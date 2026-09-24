# RESP2 Protocol Subset

## 1. Transport

Clients connect over TCP to port `6380` by default. Connections are persistent until either side
closes them. Requests may be fragmented across packets or pipelined without waiting for replies.

## 2. Requests

Version 1 accepts commands as RESP2 arrays of bulk strings. Inline commands are not required.

Example `SET name Anish` request:

```text
*3\r\n
$3\r\n
SET\r\n
$4\r\n
name\r\n
$5\r\n
Anish\r\n
```

The command name is ASCII case-insensitive. Keys and values are treated as byte strings; command
names and integer arguments must be valid ASCII.

## 3. Responses

The server emits these RESP2 types:

- Simple string: `+OK\r\n`, `+PONG\r\n`
- Error: `-ERR message\r\n`
- Integer: `:1\r\n`, `:0\r\n`, `:-1\r\n`, `:-2\r\n`
- Bulk string: `$5\r\nvalue\r\n`
- Null bulk string: `$-1\r\n`

Error messages are stable enough for humans but tests should primarily assert the RESP error type
and error category rather than complete prose.

## 4. Commands

```text
PING
SET key value
GET key
DEL key
EXISTS key
EXPIRE key seconds
TTL key
```

`seconds` is a base-10 nonnegative integer that fits in signed 64 bits and in the server's clock
duration. `EXPIRE key 0` makes the key immediately unavailable.

## 5. Parser Result

The incremental parser must distinguish:

1. `Complete(command, consumed_bytes)`
2. `NeedMoreData`
3. `ProtocolError(category)`

It must never read beyond available input, allocate directly from an untrusted declared length
before checking limits, or recurse without a strict bound.

## 6. Compatibility Claim

The goal is compatibility with `redis-cli` for the seven supported commands, not full Redis
compatibility. Unsupported commands receive `-ERR unknown command\r\n`.

