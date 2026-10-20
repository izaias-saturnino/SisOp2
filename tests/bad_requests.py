"""Sends invalid requests straight to a server and checks they're refused.

The client validates names itself, so this is the only way to make sure the
server does too. Usage: python3 bad_requests.py PORT
"""

import socket
import struct
import sys

PORT = int(sys.argv[1])

OK, ERROR = 1, 2
LOGIN, PING, UPLOAD, DOWNLOAD, DELETE = 10, 12, 14, 15, 16
LOGIN_OK, PONG = 20, 22
FILE_DATA, FILE_END = 40, 41

failures = []


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
    return socket.create_connection(("127.0.0.1", PORT), timeout=10)


for bad_user in ["..", "../alice", "a/b", "", "x" * 40]:
    with connect() as sock:
        sock.sendall(frame(LOGIN, text(bad_user)))
        expect(f"login as {bad_user!r} is refused", receive(sock)[0] == ERROR)

with connect() as sock:
    sock.sendall(frame(LOGIN, text("mallory")))
    expect("a valid login works", receive(sock)[0] == LOGIN_OK)

    for bad_name in ["..", ".", "", "../alice/big.bin", "/etc/passwd", ".sisop_state"]:
        sock.sendall(frame(DELETE, b"\x00" + text(bad_name)))
        expect(f"delete {bad_name!r} is refused", receive(sock)[0] == ERROR)
        sock.sendall(frame(DOWNLOAD, text(bad_name)))
        expect(f"download {bad_name!r} is refused", receive(sock)[0] == ERROR)

    # An upload with a bad name: the body must still be consumed.
    header = b"\x00" + text("../../escape.txt") + struct.pack(">Qq", 3, 0)
    sock.sendall(frame(UPLOAD, header) + frame(FILE_DATA, b"abc") + frame(FILE_END))
    expect("upload of ../../escape.txt is refused", receive(sock)[0] == ERROR)
    sock.sendall(frame(PING))
    expect("the connection is still usable afterwards", receive(sock)[0] == PONG)

# A frame announcing a 4 GB payload: the server must just drop the connection.
with connect() as sock:
    sock.sendall(struct.pack(">HHI", LOGIN, 0, 0xFFFFFFFF) + b"x" * 64)
    try:
        sock.recv(1)
    except OSError:
        pass

print("\n".join(failures))
sys.exit(1 if failures else 0)
