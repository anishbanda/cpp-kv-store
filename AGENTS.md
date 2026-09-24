# Project Map

This repository implements a high-performance, multithreaded, in-memory key-value server in
modern C++ for Linux. It is a focused systems project, not a complete Redis reimplementation.

Read these documents before changing code:

- `SPEC.md`: required behavior and acceptance criteria
- `ARCHITECTURE.md`: component boundaries and ownership rules
- `docs/PROTOCOL.md`: supported RESP subset and wire behavior
- `docs/CONCURRENCY.md`: threading, locking, and shutdown rules
- `TASKS.md`: ordered milestones and completion gates

## Engineering Rules

- Use C++20, CMake, and Ninja.
- Target Linux; networking uses nonblocking sockets and `epoll`.
- Prefer RAII and standard-library facilities. Do not add Boost or a networking framework.
- Do not use owning raw pointers or global mutable state.
- Keep networking, protocol, command execution, storage, and expiration concerns separate.
- The event-loop thread exclusively owns connection state and socket I/O.
- Document lock ordering and other concurrency assumptions next to the relevant code.
- Do not significantly change the documented architecture without updating its documentation.
- Compile with `-Wall -Wextra -Wpedantic -Werror`.
- Keep patches scoped to the active milestone. Do not implement future milestones early.
- Never invent benchmark results.

## Verification

Before declaring a task complete, run the applicable commands inside the Linux container:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

For concurrency or memory-sensitive changes, also run:

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

No task is complete if existing tests fail. Add tests for every behavior or bug fix.

