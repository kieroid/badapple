#!/usr/bin/env python3
"""Check whether a DWC2 core soft reset stops the core accepting writes.

m1n1 and PongoOS both bring this gadget up without ever issuing one, and both
work. Linux issues one in dwc2_core_reset() on the way into
dwc2_hsotg_core_init_disconnected(), and from that point every write to
GAHBCFG, DCTL and GOTGCTL is dropped while reads keep working, so the gadget
never raises its pullup.

Run it against a parked m1n1 whose gadget is up, which makes the before state a
known good one:

    M1N1DEVICE=/dev/ttyACM1 usbreset-probe.py

It resets the core underneath m1n1, so m1n1's own gadget will drop off USB.
That is expected: the point is the readback, not surviving it.
"""

import sys
import time
from pathlib import Path

M1N1 = Path(__file__).resolve().parent.parent / "build" / "m1n1" / "proxyclient"
sys.path.insert(0, str(M1N1))

from m1n1.setup import p  # noqa: E402

DWC2 = 0x239100000
GAHBCFG = 0x008
GRSTCTL = 0x010
GRSTCTL_CSFTRST = 1 << 0
GRSTCTL_AHBIDLE = 1 << 31

# What m1n1 leaves in GAHBCFG: burst length 7 plus DMA enable.
GAHBCFG_KNOWN = 0x0000002E


def show(label):
    print(f"{label}: GAHBCFG={p.read32(DWC2 + GAHBCFG):#010x} "
          f"GRSTCTL={p.read32(DWC2 + GRSTCTL):#010x}")


def main():
    show("before     ")

    # Prove writes work at all right now, by clearing and restoring a bit.
    p.write32(DWC2 + GAHBCFG, 0)
    cleared = p.read32(DWC2 + GAHBCFG)
    p.write32(DWC2 + GAHBCFG, GAHBCFG_KNOWN)
    restored = p.read32(DWC2 + GAHBCFG)
    print(f"write test : cleared to {cleared:#010x}, restored to {restored:#010x}")
    if restored != GAHBCFG_KNOWN:
        raise SystemExit("writes were already not working, nothing to learn here")

    print("\nissuing core soft reset")
    p.write32(DWC2 + GRSTCTL, p.read32(DWC2 + GRSTCTL) | GRSTCTL_CSFTRST)

    for _ in range(100):
        if not (p.read32(DWC2 + GRSTCTL) & GRSTCTL_CSFTRST):
            break
        time.sleep(0.01)
    else:
        print("  CSFTRST never cleared, the reset itself hangs")

    for _ in range(100):
        if p.read32(DWC2 + GRSTCTL) & GRSTCTL_AHBIDLE:
            break
        time.sleep(0.01)
    else:
        print("  AHBIdle never set")

    show("after reset")

    # The same write that worked a moment ago.
    p.write32(DWC2 + GAHBCFG, GAHBCFG_KNOWN)
    after = p.read32(DWC2 + GAHBCFG)
    print(f"write after: wrote {GAHBCFG_KNOWN:#010x}, read back {after:#010x}")

    if after == GAHBCFG_KNOWN:
        print("\nwrites still land, so the soft reset is not what stops Linux")
    else:
        print("\nwrites are dropped after the soft reset, which is exactly what "
              "Linux sees, so Linux should not issue one on this core")


if __name__ == "__main__":
    main()
