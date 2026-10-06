#!/usr/bin/env python3
"""Upload a blob to PongoOS and boot it, talking to the phone from this PC.

The jig is only needed to get as far as a PongoOS shell. From there PongoOS
brings up its own USB gadget, which does enumerate here as 05ac:4141, so the
cable can be moved to this machine once and everything after that happens over
it.

That matters for more than convenience. Booting through the jig means Linux's
dwc2 probes while the phone is attached to the RP2350, because the board is the
host that runs the exploit, so the gadget binds to it and never re-presents
itself to a different host. Going this way the cable is already here before the
kernel starts, which is the arrangement the A11 port gets for free from
checkm8.

It also cuts the number of times the phone has to come off the jig, and the
exploit gets less reliable the more that happens.

Usage:
    pcboot.py <blob>       upload and run bootm
"""

import struct
import sys
from pathlib import Path

import usb.core

VENDOR = 0x05AC
PRODUCT = 0x4141

# PongoOS's control requests, from its own scripts: 1 sets the length of the
# upload, 2 resets the upload buffer, 3 runs a shell command.
REQ_SET_LEN = 1
REQ_RESET = 2
REQ_COMMAND = 3
OUT_REQUEST = 0x21
BULK_OUT_EP = 2


def find_pongo():
    device = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
    if device is None:
        raise SystemExit(
            "no PongoOS on USB (05ac:4141). Get to a PongoOS shell with "
            "'make pongo' and move the cable from the jig to this machine.")
    device.set_configuration()
    return device


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <blob>")

    blob = Path(sys.argv[1])
    data = blob.read_bytes()
    device = find_pongo()

    print(f"upload {blob} ({len(data)} bytes)")
    device.ctrl_transfer(OUT_REQUEST, REQ_RESET, 0, 0, 0)
    device.ctrl_transfer(OUT_REQUEST, REQ_SET_LEN, 0, 0, struct.pack("I", len(data)))
    device.write(BULK_OUT_EP, data, 1000000)

    # PongoOS takes the command and jumps, so it stops servicing USB partway
    # through. A pipe error here is the handoff, not a failure.
    print("bootm")
    try:
        device.ctrl_transfer(OUT_REQUEST, REQ_COMMAND, 0, 0, b"bootm\n")
    except usb.core.USBError as error:
        print(f"  usb went away: {error}")
        print("  that is what a successful handoff looks like")


if __name__ == "__main__":
    main()
