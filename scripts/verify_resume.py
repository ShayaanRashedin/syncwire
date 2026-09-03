#!/usr/bin/env python3
"""Real TCP/CLI acceptance test and small, explicitly local performance sample.

Uses only the Python standard library. All data lives in a disposable temporary directory.
No user server, credentials, or existing files are touched.
"""

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import re
import secrets
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import zlib


HEADER = struct.Struct("!IBBHIIQQ")
CHUNK = 65536


def receive_exact(sock, count):
    result = bytearray()
    while len(result) < count:
        data = sock.recv(count - len(result))
        if not data:
            raise RuntimeError("unexpected EOF in acceptance test")
        result.extend(data)
    return bytes(result)


def receive_frame(sock):
    header = receive_exact(sock, HEADER.size)
    magic, version, kind, size, length, flags, request, transfer = HEADER.unpack(header)
    if (magic, version, size, flags) != (0x53574952, 2, 32, 0) or length > 1048576:
        raise RuntimeError("invalid frame in acceptance test")
    return kind, request, transfer, receive_exact(sock, length)


def send_frame(sock, kind, request=0, transfer=0, payload=b""):
    sock.sendall(HEADER.pack(0x53574952, 2, kind, 32, len(payload), 0, request, transfer) + payload)


def authenticate(sock, secret):
    kind, request, transfer, server_nonce = receive_frame(sock)
    assert (kind, request, transfer, len(server_nonce)) == (0x10, 0, 0, 32)
    nonce = secrets.token_bytes(32)
    proof = hmac.digest(secret, b"SyncWire-v2-client-proof" + server_nonce + nonce, "sha256")
    send_frame(sock, 0x11, payload=nonce + proof)
    kind, request, transfer, result = receive_frame(sock)
    expected = hmac.digest(secret, b"SyncWire-v2-server-proof" + server_nonce + nonce + proof, "sha256")
    assert (kind, request, transfer) == (0x12, 0, 0)
    assert hmac.compare_digest(result, b"\0" + expected)


def start_server(binary, destination, env):
    process = subprocess.Popen([str(binary), "0", str(destination), "4"], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        readable, _, _ = select.select([process.stdout], [], [], 10)
        if not readable:
            raise RuntimeError("server did not announce readiness")
        first = process.stdout.readline()
        match = re.search(r"listening on 127\.0\.0\.1:(\d+)", first)
        if not match:
            raise RuntimeError("server failed to start: " + first)
        return process, int(match.group(1))
    except BaseException:
        process.kill()
        process.communicate(timeout=5)
        raise


def stop_server(process):
    if process.poll() is None:
        process.terminate()
    try:
        output, _ = process.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate(timeout=5)
        raise RuntimeError("server did not shut down gracefully: " + output)
    if process.returncode != 0:
        raise RuntimeError("server exited unsuccessfully: " + output)
    return output


def interrupt_upload(port, secret, data, remote):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        authenticate(sock, secret)
        metadata = struct.pack("!HQI", len(remote), len(data), zlib.crc32(data)) + remote
        send_frame(sock, 0x20, 100, payload=metadata)
        kind, request, transfer, ready = receive_frame(sock)
        assert (kind, request, struct.unpack("!QI", ready)) == (0x22, 100, (0, 0))
        for offset in (0, CHUNK):
            send_frame(sock, 0x23, request, transfer, struct.pack("!Q", offset) + data[offset:offset + CHUNK])
            kind, ack_request, ack_transfer, ack = receive_frame(sock)
            assert (kind, ack_request, ack_transfer, struct.unpack("!Q", ack)[0]) == (
                0x24, request, transfer, offset + CHUNK)
        sock.sendall(HEADER.pack(0x53574952, 2, 0x23, 32, CHUNK + 8, 0, request, transfer)[:9])


def run_client(binary, port, env, *args, success=True):
    result = subprocess.run([str(binary), "127.0.0.1", str(port), *map(str, args)],
                            env=env, capture_output=True, text=True, timeout=30)
    if (result.returncode == 0) != success:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def test_automatic_retry(client_binary, server_port, env, source):
    """Drop the first transfer after one ACK; relay the retry to the same real server."""
    failures = []
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(2)
    listener.settimeout(10)
    proxy_port = listener.getsockname()[1]

    def relay_frame(source_sock, destination_sock):
        value = receive_frame(source_sock)
        send_frame(destination_sock, *value)
        return value

    def proxy():
        try:
            for attempt in range(2):
                incoming, _ = listener.accept()
                with incoming, socket.create_connection(("127.0.0.1", server_port), timeout=5) as backend:
                    incoming.settimeout(5)
                    relay_frame(backend, incoming)   # challenge
                    relay_frame(incoming, backend)   # client proof
                    relay_frame(backend, incoming)   # server proof
                    relay_frame(incoming, backend)   # upload metadata
                    relay_frame(backend, incoming)   # resume offer
                    while True:
                        kind, _, _, _ = relay_frame(incoming, backend)
                        relay_frame(backend, incoming)
                        if attempt == 0 or kind == 0x25:
                            break
        except BaseException as error:
            failures.append(error)
        finally:
            listener.close()

    worker = threading.Thread(target=proxy, daemon=True)
    worker.start()
    try:
        output = run_client(client_binary, proxy_port, env, "upload", source, "retry.bin", 300)
    finally:
        worker.join(timeout=15)
        listener.close()
    assert not worker.is_alive() and not failures, str(failures)
    assert "retry 1/2" in output and "resumed 65536 bytes" in output, output
    print("PASS: automatic reconnect re-authenticated and reused 65536 bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/debug"))
    args = parser.parse_args()
    build = args.build_dir.resolve()
    secret = secrets.token_hex(32)
    env = dict(os.environ, SYNCWIRE_PSK=secret)
    server = None
    with tempfile.TemporaryDirectory(prefix="syncwire-acceptance-") as temporary:
        root = Path(temporary)
        destination = root / "destination"
        source = root / "source.bin"
        data = bytes(range(256)) * 4096  # 1 MiB, deterministic and inexpensive.
        source.write_bytes(data)
        try:
            server, port = start_server(build / "syncwire-server", destination, env)
            interrupt_upload(port, secret.encode(), data, b"resumed.bin")
            # Kill only the child created above: no graceful checkpoint/shutdown callback.
            server.kill()
            server.communicate(timeout=5)
            assert server.returncode == -9
            server = None
            assert not (destination / "resumed.bin").exists()
            saved = list((destination / ".syncwire-partials").glob("*.part"))
            assert len(saved) == 1 and saved[0].stat().st_size == 2 * CHUNK
            server, port = start_server(build / "syncwire-server", destination, env)
            output = run_client(build / "syncwire-client", port, env, "upload", source, "resumed.bin", 200)
            assert "resumed 131072 bytes, sent 917504 bytes" in output, output
            assert hashlib.sha256((destination / "resumed.bin").read_bytes()).digest() == hashlib.sha256(data).digest()
            print("PASS: forced-restart recovery reused 131072 bytes; final SHA-256 matches")

            test_automatic_retry(build / "syncwire-client", port, env, source)
            assert (destination / "retry.bin").read_bytes() == data
            wrong = dict(env, SYNCWIRE_PSK="wrong-secret-for-negative-test")
            output = run_client(build / "syncwire-client", port, wrong, "ping", success=False)
            assert "authentication was rejected" in output and "reconnecting" not in output, output
            print("PASS: wrong secret rejected without retry")

            tree = root / "tree"
            (tree / "nested").mkdir(parents=True)
            (tree / "nested" / "file.bin").write_bytes(data)
            interrupt_upload(port, secret.encode(), data, b"nested/file.bin")
            run_client(build / "syncwire-client", port, env, "sync", tree, 400)
            output = run_client(build / "syncwire-client", port, env, "sync", tree, 500)
            assert "0 uploaded, 1 unchanged, 2 server-only" in output, output
            assert (destination / "nested" / "file.bin").read_bytes() == data
            assert not list((destination / ".syncwire-partials").glob("*.part"))
            print("PASS: interrupted nested upload recovered through sync; repeat sync transfers zero files")

            started = time.perf_counter()
            run_client(build / "syncwire-client", port, env, "upload", source, "benchmark.bin", 600)
            elapsed = time.perf_counter() - started
            assert (destination / "benchmark.bin").read_bytes() == data
            print(f"SAMPLE: local 1 MiB upload including auth/hash/fsync: {elapsed:.3f} s, {1 / elapsed:.2f} MiB/s")
            print("This is a local smoke measurement, not a WAN or scalability benchmark.")
        finally:
            if server is not None:
                stop_server(server)
    print("PASS: all real CLI/TCP acceptance checks")


if __name__ == "__main__":
    main()
