#!/usr/bin/env python3
"""Run one command on the phone's telnet shell and print the output.

The initramfs listens on 172.16.42.1:23 over the CDC-NCM gadget. telnetlib is
gone from the standard library as of Python 3.13, and all this needs is to
refuse every option the server offers, so the negotiation is inline.
"""

import socket
import sys

IAC, DONT, WONT, WILL, DO = 255, 254, 252, 251, 253

host = "172.16.42.1"
command = " ".join(sys.argv[1:]) or "uname -a"

sock = socket.create_connection((host, 23), timeout=10)
sock.settimeout(2.0)

out = bytearray()
deadline_hits = 0
sock.sendall((command + "\n").encode())

while deadline_hits < 3:
    try:
        chunk = sock.recv(4096)
    except socket.timeout:
        deadline_hits += 1
        continue
    if not chunk:
        break
    data = bytearray()
    i = 0
    while i < len(chunk):
        if chunk[i] == IAC and i + 2 < len(chunk):
            verb, option = chunk[i + 1], chunk[i + 2]
            if verb in (WILL, WONT):
                sock.sendall(bytes([IAC, DONT, option]))
            elif verb in (DO,):
                sock.sendall(bytes([IAC, WONT, option]))
            i += 3
            continue
        data.append(chunk[i])
        i += 1
    out += data

sys.stdout.write(out.decode("utf-8", "replace"))
