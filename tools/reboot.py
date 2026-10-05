#!/usr/bin/env python3
"""Reboot the phone without the button combo, whichever stage it is in.

Tries m1n1's proxy if it is up. Deliberately does not use m1n1.setup or
ProxyUtils: those allocate and pin m1n1's heap limit, which is what burns a
session. P_REBOOT is sent with no_reply, so only m1n1's receive path has to be
working.

This does not reach DFU. There is no software path into DFU on this hardware, so
the button combo is still the way there. This only saves the combo when all you
want is to get out of a parked m1n1 or a recovery prompt.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "build" / "m1n1" / "proxyclient"))

# iPhone XS Max has no home button, so it needs the three step sequence. Side
# plus Volume Down held together is the iPhone 7 era combo and does not work
# here. The first two presses must be quick taps, not holds.
FORCE_RESTART = """force restart:
    1. press and quickly release Volume Up
    2. press and quickly release Volume Down
    3. press and hold Side until it restarts, 10 to 20s"""

DFU = """then DFU:
    1. quick press Volume Up
    2. quick press Volume Down
    3. hold Side until the screen goes black, about 10s
    4. still holding Side, also hold Volume Down for 5s
    5. release Side, keep holding Volume Down about 10s"""


def m1n1_tty():
    """The m1n1 proxy is USB 1209:316d and exposes two ACM interfaces. Take the
    first, and do not confuse it with the l41ka board on 4141:4c41."""
    for node in sorted(pathlib.Path("/sys/class/tty").glob("ttyACM*")):
        device = (node / "device").resolve()
        try:
            vendor = (device.parent / "idVendor").read_text().strip()
        except OSError:
            continue
        if vendor == "1209":
            return f"/dev/{node.name}"
    return None


def try_m1n1():
    tty = m1n1_tty()
    if not tty:
        return False
    from m1n1.proxy import M1N1Proxy, UartInterface
    print(f"m1n1 proxy on {tty}, sending P_REBOOT")
    uart = UartInterface(tty, debug=False)
    M1N1Proxy(uart, debug=False).reboot()
    print("sent")
    return True


def main():
    for attempt in (try_m1n1,):
        try:
            if attempt():
                return
        except Exception as error:  # noqa: BLE001
            print(f"{attempt.__name__}: {error}")

    print("\nNothing responded, so it has to be the buttons.")
    print(FORCE_RESTART)
    print(DFU)
    print("\nIf the buttons do nothing at all, let the battery run down.")


if __name__ == "__main__":
    main()
