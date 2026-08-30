# SyncWire

SyncWire is a Linux client-server application for reliable file transfer over TCP. The project
is intentionally focused on protocol design, message framing, partial I/O, concurrency,
backpressure, resumable transfers, and failure handling.

## Current milestone

Milestone 1 establishes the wire-format foundation:

- a versioned 32-byte binary frame header;
- explicit big-endian encoding and decoding (the C++ struct is never sent directly);
- bounded payload validation;
- an incremental parser that handles fragmented and coalesced TCP data;
- dependency-free unit tests for boundary and fragmentation behavior.

The next vertical slice will add a single-client blocking PING/PONG server and client. `epoll`
will be introduced only after the protocol and blocking I/O loops are correct.

## Build on Ubuntu or Debian

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build clang clang-format clang-tidy

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

You can also build without presets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Design constraints

- C++20 and POSIX APIs on Linux.
- All multi-byte wire fields use network byte order.
- Frame and buffer sizes are bounded before allocation or dispatch.
- The parser retains incomplete data across reads and may emit multiple frames per read.
- A protocol parsing error is terminal for that parser/connection.
- Advanced transfer features are added only after the previous vertical slice is tested.

See [`docs/protocol.md`](docs/protocol.md) for the first wire-format specification.

