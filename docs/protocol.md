# SyncWire Protocol v1 (foundation)

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

Payload schemas and message-order rules will be specified as their vertical slices are added.

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

