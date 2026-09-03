# SyncWire Protocol v1

## Transport

SyncWire v1 runs over TCP. TCP is an ordered byte stream; it does not preserve message
boundaries. A receiver must therefore accumulate bytes until a complete fixed header and its
declared payload are available.

## Frame format

Every frame begins with a fixed 32-byte header. All multi-byte integers are unsigned and encoded
in big-endian (network) byte order.

| Offset | Size | Field | v1 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | `0x53574952` (`SWIR`) |
| 4 | 1 | Version | `1` |
| 5 | 1 | Message type | Must be a known v1 value |
| 6 | 2 | Header size | `32` |
| 8 | 4 | Payload length | Must not exceed the configured limit |
| 12 | 4 | Flags | Must be zero in v1 |
| 16 | 8 | Request ID | Correlates request and response |
| 24 | 8 | Transfer ID | Zero when no transfer exists yet |

The serialized size is always `32 + payload_length`. C++ object layout is never used as the wire
format because it may contain padding and uses host byte order.

## Initial message types

| Value | Type | Direction/purpose |
| ---: | --- | --- |
| `0x01` | `PING` | Client requests a liveness response |
| `0x02` | `PONG` | Server echoes the PING request ID |
| `0x10` | `AUTH_CHALLENGE` | Server supplies an authentication nonce |
| `0x11` | `AUTH_PROOF` | Client supplies its challenge response |
| `0x12` | `AUTH_RESULT` | Server reports authentication status |
| `0x20` | `UPLOAD_REQUEST` | Starts or resumes an upload |
| `0x21` | `DOWNLOAD_REQUEST` | Starts or resumes a download |
| `0x22` | `TRANSFER_READY` | Reports accepted transfer metadata/offset |
| `0x23` | `FILE_CHUNK` | Contains a byte offset followed by file data |
| `0x24` | `ACKNOWLEDGMENT` | Reports the next expected upload offset |
| `0x25` | `TRANSFER_COMPLETE` | Sender reports end of bytes |
| `0x26` | `TRANSFER_RESULT` | Receiver reports verification/commit status |
| `0x30` | `SYNC_MANIFEST` | Client describes a recursive source tree |
| `0x31` | `SYNC_PLAN` | Server requests missing or changed paths |
| `0x32` | `SYNC_COMPLETE` | Client reports all planned uploads complete |
| `0x33` | `SYNC_RESULT` | Server reports post-upload verification status |
| `0x7f` | `ERROR` | Reports a structured protocol/operation error |

Download payloads remain reserved for a later slice. Authentication and upload ordering rules
are specified below.

## Authentication exchange

Every server session authenticates before the client sends its PING, upload, or sync operation.
Authentication frames use `request_id = 0` and `transfer_id = 0`.

1. Server generates 32 random bytes with OpenSSL `RAND_bytes()` and sends them as
   `AUTH_CHALLENGE`.
2. Client generates its own 32-byte nonce and calculates HMAC-SHA256 over the ASCII domain
   string `SyncWire-v1-client-proof`, the server nonce, and the client nonce. The configured
   `SYNCWIRE_PSK` bytes are the HMAC key.
3. Client sends its 32-byte nonce followed by the 32-byte digest as `AUTH_PROOF`.
4. Server independently calculates the client digest and compares it with `CRYPTO_memcmp()`.
5. When accepted, the server calculates HMAC-SHA256 over
   `SyncWire-v1-server-proof || server-nonce || client-nonce || client-proof`.
6. Server sends `AUTH_RESULT`. Rejection is the one byte `1`; acceptance is the byte `0`
   followed by the 32-byte server proof.
7. Client calculates and constant-time verifies the server proof before sending an operation.

Secrets shorter than 16 bytes or longer than 1,024 bytes are rejected locally. A malformed
challenge, proof, or result terminates the connection. The secret is never included in a frame,
CLI argument, or log message.

Fresh 256-bit nonces from both peers prevent capture-and-replay of either proof. The distinct
proof domains provide mutual peer authentication and prevent reflecting a client proof as a
server proof. The handshake does not encrypt subsequent frames, hide metadata, or provide
forward secrecy. The reference server therefore remains bound to `127.0.0.1` until a
transport-encryption layer is added.

## PING/PONG exchange

`PING` and `PONG` have no payload and must use `transfer_id = 0`. The client chooses a nonzero
`request_id`; the server returns the same value in its `PONG`. A zero request ID, nonempty payload,
nonzero transfer ID, wrong message type, or mismatched response ID makes the exchange invalid.

Each authenticated TCP connection carries one complete PING, upload, or directory-sync session.
The server may serve many independent connections concurrently.

## Concurrent server runtime

The listening socket is non-blocking and registered with Linux `epoll`. Readiness causes the
acceptor to drain all currently available connections into a bounded in-memory queue. A fixed,
configurable worker pool removes connections from that queue and executes one complete protocol
session per connection.

The queue and worker count are bounded. When the queue is full, a newly accepted connection is
closed and counted as rejected rather than allocating an unbounded thread or buffer. PING
sessions may execute in parallel. Upload and directory-sync sessions share a destination lock so
manifest decisions, temporary-file creation, and atomic commits cannot race within one server
process. This is deliberately conservative; path-level locking can increase write concurrency
later without changing the protocol.

On shutdown, the acceptor stops admitting clients, closes queued connections, calls `shutdown()`
on active sockets to interrupt blocking I/O, and joins every worker. Accepted, completed, failed,
rejected, cancelled, and peak-queue counters are reported by the command-line server.

## Single-file upload exchange

Every upload uses one nonzero client-selected `request_id`. The initial `UPLOAD_REQUEST` has a
zero transfer ID. The server returns a nonzero transfer ID in `TRANSFER_READY`; all later frames
must carry both matching IDs. The server currently uses the request ID as the transfer ID.
Identifiers are scoped to one TCP connection, so concurrent clients may safely choose the same
request ID.

The valid message order is:

1. Client sends one `UPLOAD_REQUEST`.
2. Server sends `TRANSFER_READY`, or a rejecting `TRANSFER_RESULT` with transfer ID zero.
3. Client sends one or more contiguous `FILE_CHUNK` frames. The server responds to each with an
   `ACKNOWLEDGMENT` containing the next required offset.
4. Client sends `TRANSFER_COMPLETE` after exactly the declared byte count.
5. Server verifies the byte count and CRC-32, atomically commits the file, and sends
   `TRANSFER_RESULT`.

Zero-byte files skip step 3. Any malformed payload, mismatched ID, unexpected message, gap,
overlap, excess byte, early completion, or integrity failure terminates the transfer.

### `UPLOAD_REQUEST` payload

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 2 | Filename length | `1..255`, big-endian |
| 2 | 8 | File size | Big-endian; default maximum is 1 GiB |
| 10 | 4 | CRC-32 | IEEE CRC-32 of the complete file, big-endian |
| 14 | variable | Filename | Exactly `filename_length` bytes |

The filename is interpreted as a single destination basename. Empty names, `.`, `..`, NUL,
forward slash, and backslash are rejected. This slice cannot create remote subdirectories.

### `TRANSFER_READY` payload

The payload is empty. Its header supplies the assigned nonzero transfer ID.

### `FILE_CHUNK` payload

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 8 | Byte offset | Big-endian; must equal the next expected offset |
| 8 | variable | File data | `1..65536` bytes by default |

The entire frame remains below the general 1 MiB frame-payload limit.

### `ACKNOWLEDGMENT` payload

The payload is one 8-byte big-endian integer containing the next expected byte offset.

### `TRANSFER_COMPLETE` payload

The payload is empty. The receiver accepts it only after exactly the declared file size.

### `TRANSFER_RESULT` payload

The payload is a one-byte result code:

| Value | Meaning |
| ---: | --- |
| `0` | Success |
| `1` | Invalid request or payload |
| `2` | Invalid destination filename |
| `3` | File exceeds configured limit |
| `4` | File I/O failure |
| `5` | Unexpected frame or correlation ID |
| `6` | Noncontiguous chunk offset |
| `7` | Declared and received sizes differ |
| `8` | CRC-32 mismatch |

CRC-32 detects accidental transfer corruption; it is not a cryptographic integrity mechanism.
The session handshake authenticates the peer with HMAC-SHA256, but TLS is still required to
cryptographically protect every subsequent frame from an active network attacker.

### Atomic destination commit

The server creates an exclusive hidden `.part` file inside the destination directory and writes
only contiguous bytes to it. After `TRANSFER_COMPLETE`, it checks size and CRC-32, calls
`fsync()`, closes the file, and renames it to the requested basename on the same filesystem.
Failures remove the part file and leave the final destination untouched.

## Directory synchronization exchange

Directory synchronization reuses the verified upload exchange for each file selected by the
server. The client chooses one nonzero synchronization request ID. Control frames use that ID and
`transfer_id = 0`; planned uploads use consecutive request IDs beginning at
`sync_request_id + 1`.

The message order is:

1. Client recursively scans its source and sends `SYNC_MANIFEST`.
2. Server scans its destination and sends `SYNC_PLAN`.
3. Client performs one complete upload exchange for every planned path, in plan order.
4. Client sends an empty `SYNC_COMPLETE`.
5. Server scans the destination again, verifies every source file, and sends `SYNC_RESULT`.

Manifests and plans are strictly sorted by normalized relative path. Duplicates, unsorted paths,
absolute paths, empty components, `.`, `..`, backslashes, and NUL bytes are invalid. Scanners do
not follow symlinks. The receiver also rejects an upload when any existing destination parent is
a symlink, preventing a nested path from escaping the configured root.

### `SYNC_MANIFEST` payload

The payload begins with a 4-byte big-endian file count. Each record then contains:

| Size | Field | Rule |
| ---: | --- | --- |
| 8 | File size | Big-endian |
| 4 | CRC-32 | Big-endian |
| 2 | Relative path length | Big-endian; `1..1024` |
| variable | Relative path | Normalized UTF-8-compatible path bytes |

The default limits are 4,096 regular files, 1 GiB per file, and the general 1 MiB frame payload.
The sender rejects a tree whose encoded manifest exceeds those limits.

### `SYNC_PLAN` payload

The first 12 bytes contain three big-endian 32-bit counts: paths to upload, unchanged source
files, and files found only on the server. Each upload path is encoded as a 2-byte length followed
by its path bytes. The paths are strictly sorted and must all exist in the source manifest.

A path is unchanged only when both its size and CRC-32 match. Missing paths and paths with either
value changed are uploaded. Server-only paths are reported but never deleted in v1.

### `SYNC_COMPLETE` and `SYNC_RESULT`

`SYNC_COMPLETE` has no payload. `SYNC_RESULT` contains one byte: `0` for successful destination
verification or `1` when a post-upload scan still differs from the source manifest. Server-only
files do not cause verification failure.

## Incremental parsing

For each connection, the receiver:

1. Appends newly received bytes to a bounded input buffer.
2. Waits if fewer than 32 bytes are available.
3. Decodes and validates the header without allocating from untrusted lengths.
4. Waits if the full `32 + payload_length` bytes are not available.
5. Emits one frame and continues, because the buffer may contain more frames.
6. Treats an invalid header or buffer-limit violation as terminal for the connection.

This behavior is tested with every split point, byte-at-a-time input, multiple frames in one read,
truncated frames, and malformed headers.

