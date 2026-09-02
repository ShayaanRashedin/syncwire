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
| `0x7f` | `ERROR` | Reports a structured protocol/operation error |

Authentication and download payloads remain reserved for later slices. The upload payloads and
ordering rules are specified below.

## PING/PONG exchange

`PING` and `PONG` have no payload and must use `transfer_id = 0`. The client chooses a nonzero
`request_id`; the server returns the same value in its `PONG`. A zero request ID, nonempty payload,
nonzero transfer ID, wrong message type, or mismatched response ID makes the exchange invalid.

The blocking reference implementation reads exactly one complete frame and serves one client.
This deliberately isolates protocol correctness and partial-I/O behavior before non-blocking
connection management is introduced.

## Single-file upload exchange

Every upload uses one nonzero client-selected `request_id`. The initial `UPLOAD_REQUEST` has a
zero transfer ID. The server returns a nonzero transfer ID in `TRANSFER_READY`; all later frames
must carry both matching IDs. In the blocking reference slice the server uses the request ID as
the transfer ID. A later concurrent implementation may allocate transfer IDs independently.

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

CRC-32 detects accidental transfer corruption; it is not an authentication mechanism. A later
authenticated protocol must use a cryptographic message authentication code.

### Atomic destination commit

The server creates an exclusive hidden `.part` file inside the destination directory and writes
only contiguous bytes to it. After `TRANSFER_COMPLETE`, it checks size and CRC-32, calls
`fsync()`, closes the file, and renames it to the requested basename on the same filesystem.
Failures remove the part file and leave the final destination untouched.

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
