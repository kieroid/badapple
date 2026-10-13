#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
"""Upload an image to a pwned DFU device and run it.

The jig exploits the phone's bootrom over USB and leaves it in DFU with a
patched request handler. That handler keeps the standard DFU download request
and adds two of its own, so sending an image is DFU_DNLOAD in chunks, a zero
length DNLOAD to end the transfer, and then request 8 to jump to what was just
uploaded. Request 7 demotes the chip, which we do not need.

A pwned device is the usual Apple DFU product id with PWND in its serial, for
instance:

    CPID:8020 CPRV:11 CPFM:03 SCEP:01 BDID:1A ECID:... PWND:[usbliter8]

The image is the patched iBSS. The bootrom jumps to it without checking a
signature, which is what puts the phone in recovery mode with DRAM up.

Usage:
    dfuboot.py <image>      upload and jump to it
"""

import sys
import time
from pathlib import Path

import usb.core

DFU_PRODUCT = 0x1227

# 1 and 6 are DFU_DNLOAD and DFU_ABORT from the DFU spec. 8 is the exploit
# handler's own "jump to the uploaded image".
REQ_DNLOAD = 1
REQ_ABORT = 6
REQ_BOOT = 8
OUT_REQUEST = 0x21

# What the handler accepts in one control transfer.
CHUNK = 0x800


def find_pwned():
    for attempt in range(20):
        for device in usb.core.find(find_all=True, idProduct=DFU_PRODUCT):
            try:
                serial = device.serial_number
            except (usb.core.USBError, NotImplementedError):
                continue
            if serial and "PWND:[" in serial:
                return device, serial
        if attempt == 0:
            print("  waiting for a pwned DFU device")
        time.sleep(1.0)

    raise SystemExit(
        "no pwned DFU device (05ac:1227 with PWND in its serial). Put the "
        "phone in DFU on the jig, wait for the exploit, then move the cable "
        "to this machine.")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <image>")

    image = Path(sys.argv[1])
    data = image.read_bytes()

    device, serial = find_pwned()
    print(f"  {serial}")

    print(f"upload {image} ({len(data)} bytes)")
    for offset in range(0, len(data), CHUNK):
        device.ctrl_transfer(OUT_REQUEST, REQ_DNLOAD, 0, 0,
                             data[offset:offset + CHUNK], 5000)
    device.ctrl_transfer(OUT_REQUEST, REQ_DNLOAD, 0, 0, None, 1000)

    # The image takes over before the status stage comes back, so a pipe error
    # from either of these is the jump happening, not a failure.
    print("boot")
    for request in (REQ_BOOT, REQ_ABORT):
        try:
            device.ctrl_transfer(OUT_REQUEST, request, 0, 0, None, 1000)
        except usb.core.USBError as error:
            print(f"  usb went away: {error}")
            break


if __name__ == "__main__":
    main()
