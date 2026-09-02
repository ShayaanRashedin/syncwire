# SyncWire

SyncWire is a Linux client-server application for reliable file transfer over TCP. The project
is intentionally focused on protocol design, message framing, partial I/O, concurrency,
backpressure, resumable transfers, and failure handling.

## Implemented milestones

### 1. Wire-format foundation

- a versioned 32-byte binary frame header;
- explicit big-endian encoding and decoding (the C++ struct is never sent directly);
- bounded payload validation;
- an incremental parser that handles fragmented and coalesced TCP data;
- dependency-free unit tests for boundary and fragmentation behavior.

### 2. Blocking PING/PONG vertical slice

- move-only RAII ownership for Linux file descriptors;
- `send_all()` and `recv_exact()` loops for partial socket I/O and `EINTR`;
- `MSG_NOSIGNAL` protection when writing to disconnected peers;
- bounded frame reads that validate a header before allocating its payload;
- a single-client blocking TCP server and command-line client;
- request/response correlation through a nonzero request ID;
- tests for ownership, early disconnects, invalid frames, wrong message types, and mismatched IDs;
- GitHub Actions build and test automation on Ubuntu.

The blocking implementation is intentionally small and temporary. The next vertical slice will
introduce non-blocking sockets, level-triggered `epoll`, per-connection state, and output queues.

## Build on Ubuntu or Debian

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build clang clang-format clang-tidy

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Run the PING/PONG slice

Start the single-client server in one terminal:

```bash
./build/debug/syncwire-server 4040
```

Run the client in a second terminal:

```bash
./build/debug/syncwire-client 127.0.0.1 4040 42
```

The client must report `PONG received for request 42`; the server exits after serving that one
request. Port `0` may be passed to the server to let Linux choose an available port.

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

