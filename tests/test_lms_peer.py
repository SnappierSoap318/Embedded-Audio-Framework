"""A simulated SlimProto server and separate HTTP PCM socket; no external server."""
import socket
import struct
import subprocess
import sys
import threading


def exact(sock, n):
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise RuntimeError("Unexpected peer disconnect")
        out += chunk
    return out


def packet(sock):
    header = exact(sock, 8)
    return header[:4], exact(sock, struct.unpack(">I", header[4:])[0])


def send_packet(sock, body):
    wire = struct.pack(">H", len(body)) + body
    for offset in range(0, len(wire), 7):
        sock.sendall(wire[offset:offset + 7])


def case(width, big, failure=None, autostart=1):
    with socket.socket() as control, socket.socket() as http:
        control.bind(("127.0.0.1", 0)); control.listen(); control.settimeout(10)
        http.bind(("127.0.0.1", 0)); http.listen(); http.settimeout(10)
        process = subprocess.Popen([sys.argv[1], str(control.getsockname()[1]),
                                    "fail" if failure else "ok"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        errors = []
        try:
            with control.accept()[0] as peer:
                peer.settimeout(10)
                opcode, hello = packet(peer)
                assert opcode == b"HELO" and b"pcm" in hello
                request = b"GET /stream.pcm HTTP/1.0\r\nHost: localhost\r\n\r\n"
                start = bytearray(28)
                start[:4] = b"strm"; start[4:11] = bytes([ord('s'), ord('0') + autostart, ord('p'),
                    ord('0') + width - 1, ord('4'), ord('2'), ord('0') if big else ord('1')])
                struct.pack_into(">H", start, 22, http.getsockname()[1])
                send_packet(peer, bytes(start) + request)
                with http.accept()[0] as stream:
                    stream.settimeout(10)
                    received = b""
                    while not received.endswith(b"\r\n\r\n"):
                        received += stream.recv(256)
                    assert received == request
                    raw = b"".join(((i % 101 - 50) * (1 << (8 * (width - 2)))).to_bytes(
                        width, "big" if big else "little", signed=True) for i in range(2050))
                    if failure == "chunked":
                        header = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        raw = b""
                    elif failure in ("gzip", "icy"):
                        field = b"Content-Encoding: gzip" if failure == "gzip" else b"icy-metaint: 16"
                        header = b"HTTP/1.1 200 OK\r\n" + field + b"\r\n\r\n"
                        raw = b""
                    else:
                        header = f"HTTP/1.0 200 OK\r\nContent-Length: {len(raw)}\r\n\r\n".encode()
                    stream.sendall(header[:9]); stream.sendall(header[9:])
                    if failure == "truncated":
                        raw = raw[:-1]
                    # Producer runs while control messages are handled independently.
                    def write_body():
                        try:
                            for i in range(0, len(raw), 17):
                                stream.sendall(raw[i:i + 17])
                            stream.shutdown(socket.SHUT_WR)
                        except Exception as e:
                            errors.append(e)
                    sender = threading.Thread(target=write_body)
                    sender.start()
                    ping = bytearray(28); ping[:5] = b"strmt"
                    struct.pack_into(">I", ping, 18, 0x12345678)
                    send_packet(peer, ping)
                    if not failure:
                        saw_ping = saw_response = saw_done = False
                        ready = resumed = paused = False
                        started = 0
                        while not saw_done:
                            opcode, body = packet(peer)
                            if opcode == b"RESP":
                                saw_response = body == header
                                if autostart >= 2:
                                    send_packet(peer, b"cont" + bytes(5))
                            if opcode == b"STAT":
                                if body[:4] == b"STMt":
                                    saw_ping |= body[47:51] == b"\x12\x34\x56\x78"
                                if body[:4] == b"STMl":
                                    assert autostart in (0, 2) and not ready
                                    ready = True
                                    release = bytearray(28); release[:5] = b"strmu"
                                    send_packet(peer, release)
                                if body[:4] == b"STMr":
                                    assert ready or paused
                                    resumed = True
                                if body[:4] == b"STMs":
                                    started += 1
                                    pause = bytearray(28); pause[:5] = b"strmp"
                                    send_packet(peer, pause)
                                    assert autostart in (1, 3) or resumed
                                    assert struct.unpack_from(">II", body, 29) == (4096, 512)
                                    assert struct.unpack_from(">I", body, 37)[0] == 1
                                    assert struct.unpack_from(">I", body, 43)[0] == 1234
                                if body[:4] == b"STMp":
                                    paused = True
                                    release = bytearray(28); release[:5] = b"strmu"
                                    send_packet(peer, release)
                                saw_done |= body[:4] == b"STMd"
                        assert saw_ping and saw_response and started == 1 and paused
                        assert ready == (autostart in (0, 2))
                    sender.join(10)
                    assert not sender.is_alive() and not errors, errors
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, (stdout, stderr)
                assert "LMS peer PASS" in stdout
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()

def rejected_gate(command):
    with socket.socket() as control, socket.socket() as http:
        control.bind(("127.0.0.1", 0)); control.listen(); control.settimeout(10)
        http.bind(("127.0.0.1", 0)); http.listen(); http.settimeout(10)
        process = subprocess.Popen([sys.argv[1], str(control.getsockname()[1]), "fail"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with control.accept()[0] as peer:
                peer.settimeout(10)
                assert packet(peer)[0] == b"HELO"
                start = bytearray(28); start[:11] = b"strms3p1421"
                struct.pack_into(">H", start, 22, http.getsockname()[1])
                send_packet(peer, start + b"GET / HTTP/1.0\r\n\r\n")
                with http.accept()[0] as stream:
                    stream.settimeout(10)
                    request = b""
                    while not request.endswith(b"\r\n\r\n"):
                        request += stream.recv(256)
                    stream.sendall(b"HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\n")
                    while packet(peer)[0] != b"RESP":
                        pass
                    send_packet(peer, command)
                    stdout, stderr = process.communicate(timeout=10)
                    assert process.returncode == 0, (stdout, stderr)
        finally:
            if process.poll() is None:
                process.kill(); process.communicate()


for width, big in [(2, False), (3, True), (4, False)]:
    case(width, big)
case(2, False, "truncated")
case(2, False, "chunked")
case(2, False, "gzip")
case(2, False, "icy")
for autostart in (0, 2, 3):
    case(2, False, autostart=autostart)

# Reject malformed/unsupported continuation and scheduled output starts.
rejected_gate(b"cont" + bytes(4))
rejected_gate(b"cont" + struct.pack(">IB", 16, 0))
scheduled = bytearray(28); scheduled[:5] = b"strmu"
struct.pack_into(">I", scheduled, 18, 100)
rejected_gate(scheduled)
