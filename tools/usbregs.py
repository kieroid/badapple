#!/usr/bin/env python3
"""Dump the USB complex, the AUSB PHY and the DWC2 core from a parked m1n1.

Why this exists: m1n1's own USB gadget enumerates on the host from this exact
silicon, and Linux's does not. Rather than guess at register semantics one DFU
cycle at a time, read what a working setup actually looks like and diff.

Run it with m1n1 sitting in its proxy and the cable moved to this machine, so
the values are those of a gadget that a host has really enumerated:

    chain.py boot build/m1n1/build/m1n1.bin
    usbregs.py

The register names and offsets are DWC2's, matching drivers/usb/dwc2/hw.h, so
the output lines up with what the Linux driver prints.
"""

import sys
from pathlib import Path

M1N1 = Path(__file__).resolve().parent.parent / "build" / "m1n1" / "proxyclient"
sys.path.insert(0, str(M1N1))

from m1n1.setup import p  # noqa: E402

# From the ADT: /arm-io/usb-complex, with otgphyctrl at +0x60 and usb-device
# at +0x100000. The device tree uses the same numbers.
COMPLEX = 0x239000000
PHY = COMPLEX + 0x60
DWC2 = 0x239100000

COMPLEX_REGS = [
    ("USBX_CTL", 0x00),
    ("WIDGET0_REMAP_CTL", 0x28),
    ("WIDGET1_REMAP_CTL", 0x38),
    ("WIDGET2_REMAP_CTL", 0x48),
]

PHY_REGS = [
    ("OTG_CTL", 0x00),
    ("OTG_SIG", 0x04),
    ("OTG_CFG0", 0x08),
    ("OTG_CFG1", 0x0C),
]

DWC2_REGS = [
    ("GOTGCTL", 0x000),
    ("GOTGINT", 0x004),
    ("GAHBCFG", 0x008),
    ("GUSBCFG", 0x00C),
    ("GRSTCTL", 0x010),
    ("GINTSTS", 0x014),
    ("GINTMSK", 0x018),
    ("GRXFSIZ", 0x024),
    ("GNPTXFSIZ", 0x028),
    ("GSNPSID", 0x040),
    ("GHWCFG1", 0x044),
    ("GHWCFG2", 0x048),
    ("GHWCFG3", 0x04C),
    ("GHWCFG4", 0x050),
    ("DCFG", 0x800),
    ("DCTL", 0x804),
    ("DSTS", 0x808),
    ("DIEPMSK", 0x810),
    ("DOEPMSK", 0x814),
    ("DAINT", 0x818),
    ("DAINTMSK", 0x81C),
    ("DIEPCTL0", 0x900),
    ("DOEPCTL0", 0xB00),
    ("PCGCTL", 0xE00),
]


def dump(label, base, regs):
    print(f"\n{label} @ {base:#x}")
    for name, offset in regs:
        print(f"  {name:<18} +{offset:#05x} = {p.read32(base + offset):#010x}")


def main():
    dump("usb complex", COMPLEX, COMPLEX_REGS)
    dump("ausb phy", PHY, PHY_REGS)
    dump("dwc2", DWC2, DWC2_REGS)

    # The three values Linux cannot explain, called out so the diff is obvious.
    dctl = p.read32(DWC2 + 0x804)
    print(f"\nsoft disconnect: {'set' if dctl & (1 << 1) else 'clear'}")
    print(f"current mode:    {'host' if p.read32(DWC2 + 0x014) & 1 else 'device'}")


if __name__ == "__main__":
    main()
