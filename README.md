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

### 3. Verified single-file upload

- an explicit upload state machine built from six framed message types;
- portable binary payload codecs for metadata, chunks, acknowledgments, and results;
- bounded 64 KiB chunks and a configurable 1 GiB default file-size limit;
- client-side CRC-32 preflight and server-side streaming integrity verification;
- contiguous-offset enforcement with an acknowledgment after every chunk;
- basename-only remote paths that reject absolute paths and traversal attempts;
- exclusive `.part` files, `fsync()`, checksum verification, and atomic rename on success;
- cleanup and structured rejection for malformed requests, bad offsets, size mismatches, and
  checksum failures;
- end-to-end socket tests for successful multi-chunk upload and failure cleanup.

### 4. Incremental directory synchronization

- recursive source and destination scans with sorted relative-path manifests;
- per-file size and CRC-32 metadata with bounded entry and payload counts;
- a manifest/plan exchange that uploads only missing or changed files;
- multiple verified file transfers over one TCP connection;
- safe nested destination paths with symlink-parent rejection;
- a second destination scan that verifies the source state after all planned uploads;
- explicit unchanged and server-only counts; server-only files are preserved by design;
- symlinks are skipped rather than followed during recursive scans;
- end-to-end tests for recursive upload, zero-work repeat sync, and symlink containment.

The implementation remains deliberately single-client and blocking. This makes synchronization
decisions independently testable before the session is moved behind non-blocking sockets,
`epoll`, per-connection state, and output queues.

## Build on Ubuntu or Debian

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build clang clang-format clang-tidy

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Run the blocking vertical slices

Start the single-client server in one terminal:

```bash
./build/debug/syncwire-server 4040 received
```

Run the client in a second terminal:

```bash
./build/debug/syncwire-client 127.0.0.1 4040 ping 42
```

The client must report `PONG received for request 42`; the server exits after serving that one
request. Port `0` may be passed to the server to let Linux choose an available port.

To upload a file, restart the one-request server and run:

```bash
./build/debug/syncwire-server 4040 received
./build/debug/syncwire-client 127.0.0.1 4040 upload ./example.bin stored.bin 43
cmp ./example.bin ./received/stored.bin
```

The remote name is optional and defaults to the source basename. It must be a plain filename;
directory components are intentionally rejected in this slice. A successful server commits the
file only after its declared size and CRC-32 match.

To synchronize a directory tree, prepare a source and restart the server:

```bash
mkdir -p /tmp/syncwire-source/nested
printf 'alpha\n' > /tmp/syncwire-source/a.txt
printf 'beta\n' > /tmp/syncwire-source/nested/b.txt

./build/debug/syncwire-server 4040 received
./build/debug/syncwire-client 127.0.0.1 4040 sync /tmp/syncwire-source 100
```

Run the server and client again with a new request ID. Unchanged source files should be reported
without being uploaded. Files that exist only on the server are counted and preserved; deletion
semantics require an explicit future opt-in and are not part of this slice.

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
- Upload filenames are basenames, file chunks are contiguous, and incomplete files are never
  exposed at their final path.
- Directory synchronization never follows symlinks and never deletes server-only files.
- The parser retains incomplete data across reads and may emit multiple frames per read.
- A protocol parsing error is terminal for that parser/connection.
- Advanced transfer features are added only after the previous vertical slice is tested.

See [`docs/protocol.md`](docs/protocol.md) for the wire-format and transfer-state specification.
