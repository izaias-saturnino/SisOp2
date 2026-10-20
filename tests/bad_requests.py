"""Sends invalid requests straight to a server and checks they're refused.

The client validates names itself, so this is the only way to make sure the
server does too. Usage: python3 bad_requests.py PORT CA_FILE
"""

import socket
import ssl
import struct
import sys

PORT = int(sys.argv[1])
CA_FILE = sys.argv[2]

OK, ERROR = 1, 2
LOGIN, PING, UPLOAD, DOWNLOAD, DELETE = 10, 12, 14, 15, 16
LOGIN_OK, PONG = 20, 22
FILE_DATA, FILE_END = 40, 41
JOIN, REPLICA_ATTACH = 50, 54

failures = []
tls = ssl.create_default_context(cafile=CA_FILE)


def frame(msg_type, payload=b""):
    return struct.pack(">HHI", msg_type, 0, len(payload)) + payload


def text(value):
    data = value.encode()
    return struct.pack(">H", len(data)) + data


def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data += chunk
    return data


def receive(sock):
    msg_type, _, length = struct.unpack(">HHI", recv_exact(sock, 8))
    return msg_type, recv_exact(sock, length)


def expect(description, condition):
    if not condition:
        failures.append(description)


def connect():
    raw = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    return tls.wrap_socket(raw, server_hostname="127.0.0.1")


def login(user, password):
    with connect() as sock:
        sock.sendall(frame(LOGIN, text(user) + text(password)))
        return receive(sock)[0]


for bad_user in ["..", "../alice", "a/b", "", "x" * 40]:
    expect(f"login as {bad_user!r} is refused", login(bad_user, "long-enough") == ERROR)
expect("a too short password is refused", login("shorty", "abc") == ERROR)
expect("a wrong password is refused", login("alice", "not-the-password") == ERROR)

with connect() as sock:
    sock.sendall(frame(LOGIN, text("mallory") + text("mallory-pass")))
    expect("a valid login works", receive(sock)[0] == LOGIN_OK)

    bad_paths = ["..", ".", "", "../alice/big.bin", "/etc/passwd", ".sisop_state", "a/../b",
                 "a//b", "a/./b", "docs/.sisop_x", "a/", "a/" * 40 + "b"]
    for bad_path in bad_paths:
        sock.sendall(frame(DELETE, b"\x00" + text(bad_path)))
        expect(f"delete {bad_path!r} is refused", receive(sock)[0] == ERROR)
        sock.sendall(frame(DOWNLOAD, text(bad_path)))
        expect(f"download {bad_path!r} is refused", receive(sock)[0] == ERROR)

    # An upload with a bad name: the body must still be consumed.
    header = b"\x00" + text("../../escape.txt") + struct.pack(">Qq", 3, 0)
    sock.sendall(frame(UPLOAD, header) + frame(FILE_DATA, b"abc") + frame(FILE_END))
    expect("upload of ../../escape.txt is refused", receive(sock)[0] == ERROR)
    sock.sendall(frame(PING))
    expect("the connection is still usable afterwards", receive(sock)[0] == PONG)

# Cluster messages need a server certificate; this connection has none.
for name, msg in [("Join", frame(JOIN, text("127.0.0.1") + struct.pack(">H", 1))),
                  ("ReplicaAttach", frame(REPLICA_ATTACH, b"\x00" * 16))]:
    with connect() as sock:
        sock.sendall(msg)
        expect(f"{name} without a server certificate is refused", receive(sock)[0] == ERROR)

# Plain TCP, no TLS: the server must not answer the login.
with socket.create_connection(("127.0.0.1", PORT), timeout=10) as raw:
    raw.sendall(frame(LOGIN, text("alice") + text("alice-pass1")))
    try:
        reply = raw.recv(8)
    except OSError:
        reply = b""
    expect("a plaintext connection gets no answer", not reply.startswith(struct.pack(">H", LOGIN_OK)))

# A frame announcing a 4 GB payload: the server must just drop the connection.
with connect() as sock:
    sock.sendall(struct.pack(">HHI", LOGIN, 0, 0xFFFFFFFF) + b"x" * 64)
    try:
        sock.recv(1)
    except OSError:
        pass

print("\n".join(failures))
sys.exit(1 if failures else 0)
