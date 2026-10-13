#!/usr/bin/env python3
"""Run one command on the phone's telnet shell and print the output.

The initramfs listens on 172.16.42.1:23 over the USB gadget. telnetlib left the
standard library in Python 3.13, and busybox telnetd is happy with its options
ignored rather than refused, so stripping the IAC triplets out of what comes
back is the whole protocol.
"""

import re
import socket
import sys

IAC = re.compile(rb"\xff[\xfb-\xfe].")

command = " ".join(sys.argv[1:]) or "uname -a"

sock = socket.create_connection(("172.16.42.1", 23), timeout=10)
sock.settimeout(2.0)
sock.sendall((command + "\n").encode())

out = bytearray()
while True:
    try:
        chunk = sock.recv(4096)
    except socket.timeout:
        break
    if not chunk:
        break
    out += IAC.sub(b"", chunk)

sys.stdout.write(out.decode("utf-8", "replace"))
