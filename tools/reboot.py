#!/usr/bin/env python3
"""Reboot the phone without the button combo, whichever stage it is in.

Tries, in order:

  1. m1n1's proxy, if it is up. Deliberately does not use m1n1.setup or
     ProxyUtils: those allocate and pin m1n1's heap limit, which is what burns
     a session. P_REBOOT is sent with no_reply, so only m1n1's receive path has
     to be working.
  2. l41ka, if the phone is on the jig and in recovery.

Neither reaches DFU. There is no software path into DFU on this hardware, so
the button combo is still the way there. This only saves the combo when all you
want is to get out of a parked m1n1 or a recovery prompt.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "build" / "m1n1" / "proxyclient"))
sys.path.insert(0, str(ROOT / "build" / "l41ka" / "scripts"))

COMBO = ("Hold Side + Volume Down for 10s, release Side, keep holding\n"
         "  Volume Down about 5s more. Black screen means DFU.")


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


def try_l41ka():
    import laikadbg as L
    client = L.LaikaClient()
    try:
        state = L.connection_from_payload(
            client.request(L.PI_POLL_FOR_DEVICE, timeout_ms=20000))
        stage = state["device_type"] if state["open"] else "none"
        print(f"l41ka sees: {stage}")
        if stage != "recovery":
            return False
        client.request(L.DEVICE_RECOVERY_REBOOT, timeout_ms=20000)
        print("sent")
        return True
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass


def main():
    for attempt in (try_m1n1, try_l41ka):
        try:
            if attempt():
                return
        except Exception as error:  # noqa: BLE001
            print(f"{attempt.__name__}: {error}")

    print("\nNothing responded, so it has to be the buttons:")
    print(" ", COMBO)


if __name__ == "__main__":
    main()
