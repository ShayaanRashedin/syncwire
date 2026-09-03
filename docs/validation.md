# Repeatable validation and demo

Run from the repository root in the Linux VM. Upgrade both server and client to protocol v2.

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --verbose
python3 scripts/verify_resume.py --build-dir build/debug
```

The acceptance script uses only Python's standard library and runs independently of any server
you may already have on port 4040. It creates a temporary source, destination, and random PSK,
binds an ephemeral loopback port, and manages only its own server processes.

Expected evidence:

- Interrupt after 131,072 acknowledged bytes, then forcibly kill and restart the test server.
- Recover a 1 MiB upload with `resumed 131072 bytes, sent 917504 bytes`.
- Verify the complete destination with an independent SHA-256 calculation.
- Drop a separate transfer with a deterministic TCP proxy; observe a new authentication
  handshake and automatic client retry reusing 65,536 bytes.
- Reject a wrong PSK without retrying authentication failure.
- Recover a partial nested file through directory synchronization; the next sync uploads zero files.
- Leave no partials for completed identities and shut down the test servers cleanly.

## Negative-case unit tests

The existing protocol, authentication, transfer, containment and concurrency suites are retained.
Resume tests add matching/empty/complete prefixes, a mid-header disconnect, corrupt partial data,
source changes, oversized partials, state-byte/count budgets, symlink/hardlink rejection,
reserved-namespace validation, and strict resume payload encoding.

The concurrent suite repeats the silent-client shutdown case 128 times to exercise sockets moving
from the pending queue into active workers. Registration and cancellation share the same mutex
as the shutdown scan, so a late worker cannot start blocking I/O after shutdown missed its socket.

CTest reports one executable; its verbose output reports the number of individual assertions.
One passing CTest entry therefore does not mean there is only one test case.

## Release and sanitizer builds

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release --verbose
python3 scripts/verify_resume.py --build-dir build/release

cmake -S . -B build/sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSYNCWIRE_SANITIZERS=ON
cmake --build build/sanitize
ctest --test-dir build/sanitize --output-on-failure
```

CI compiles Debug, Release and ASan/UBSan builds with warnings treated as errors. The real
CLI/TCP acceptance script runs against Debug and Release. Sanitizers exercise the C++ suite;
the subprocess-based acceptance script is not run in the sanitizer job.

## Performance evidence without inflated claims

The demo prints a measured 1 MiB local upload duration and MiB/s, including subprocess startup,
authentication, source CRC preflight, file transfer, and synchronous disk writes. Record OS,
compiler, build preset, VM allocation, and storage when presenting results. Repeat runs and
report a median if you use these numbers in a portfolio. Do not label a Debug/CI measurement as
production throughput or extrapolate loopback results to WAN links.

## Manual reconnect

Start the server as described in the README, interrupt an upload, and rerun the same command:

```bash
./build/debug/syncwire-client 127.0.0.1 4040 upload ./large.bin recovered.bin 900 --retries 0
# After an interruption, reconnect with the same source and remote name; request ID may change.
./build/debug/syncwire-client 127.0.0.1 4040 upload ./large.bin recovered.bin 901
cmp ./large.bin ./received/recovered.bin
```

The final line's exit code should be zero. Until commit, any previous destination file remains
in place. A changed source identity leaves its older partial behind; stop the server before
manually inspecting/removing obsolete files inside that destination's `.syncwire-partials`.
Deleting a partial loses only that identity's resume progress, not its final committed file.
