#!/usr/bin/env python3
"""Read an Apple iOS DeviceTree dumped to JSON by "ipsw dtree --json" and print
the parts that matter when writing a Linux DTSI for the same SoC.

Every reg address is an offset from the parent node's own base, and that nests.
/arm-io sets the starting base from its ranges property (0x200000000 on T8020),
a direct child like usb-complex at 0x39000000 therefore sits at 0x239000000, and
usb-complex's own child at 0x100000 sits at 0x239100000 rather than 0x200100000.
Getting this wrong is quiet rather than loud, so the base is accumulated down
the tree instead of being added once at the top.

Usage:
    iosdt.py d331pap.json                list every node with reg and compatible
    iosdt.py d331pap.json uart           only nodes whose path matches "uart"
"""

import json
import re
import sys


def children(node):
    for group in node.get("children", []) or []:
        for name, child in group.items():
            yield name, child


def walk(node, path="/"):
    yield path, node
    for name, child in children(node):
        yield from walk(child, path + name + "/")


def as_u64_pairs(value):
    """Normalise the shapes ipsw emits for reg into (addr, size) tuples.
    Decoded nodes give a list of {"addr":..,"size":..}; anything ipsw could not
    decode stays a base64 blob of little endian 64 bit words."""
    if value is None:
        return []
    if isinstance(value, str):
        import base64
        raw = base64.b64decode(value)
        words = [int.from_bytes(raw[i:i + 8], "little")
                 for i in range(0, len(raw) - 7, 8)]
        return list(zip(words[0::2], words[1::2]))
    if isinstance(value, list):
        if all(isinstance(v, dict) for v in value):
            return [(v.get("addr", 0), v.get("size", 0)) for v in value]
    if isinstance(value, dict):
        return [(value.get("addr", 0), value.get("size", 0))]
    return []


def ranges_base(node):
    """arm-io ranges is a list of (child, parent, size) triplets. The parent
    address of the first triplet is where the SoC register window starts."""
    words = [a for pair in as_u64_pairs(node.get("ranges")) for a in pair]
    return words[1] if len(words) >= 2 else 0


def resolve(node, path="/", base=0):
    """Yield (path, node, base, regs) with base already accumulated from every
    ancestor, so regs[i][0] + base is the absolute address."""
    if path == "/arm-io/":
        base = ranges_base(node)
    regs = as_u64_pairs(node.get("reg"))
    yield path, node, base, regs
    # Children are addressed from this node's own first reg, once we are inside
    # the SoC window. Above /arm-io the addresses are already absolute, and
    # /arm-io itself only passes down the window base: its own reg list is the
    # scattered CPU and PMGR regions, which are not a window for anything.
    child_base = base
    if regs and path.startswith("/arm-io/") and path != "/arm-io/":
        child_base = base + regs[0][0]
    for name, child in children(node):
        yield from resolve(child, path + name + "/", child_base)


def selfcheck():
    """The nesting is the only real logic in here and it fails silently, so
    pin it to addresses taken from the T8020 tree by hand."""
    def kid(name, node):
        return {"children": [{name: node}]}

    tree = {
        "ranges": [{"addr": 0x0, "size": 0x200000000},
                   {"addr": 0x100000000, "size": 0}],
        "reg": [{"addr": 0x210F00000, "size": 0x50000}],
        "children": [{
            "aic": {"reg": [{"addr": 0x3B100000, "size": 0x8000}]},
            "usb-complex": dict(
                reg=[{"addr": 0x39000000, "size": 0x100}],
                **kid("usb-device", {"reg": [{"addr": 0x100000, "size": 0x10000}]}),
            ),
        }],
    }
    got = {path: regs[0][0] + base
           for path, _, base, regs in resolve(kid("arm-io", tree)) if regs}

    assert got["/arm-io/aic/"] == 0x23B100000, hex(got["/arm-io/aic/"])
    assert got["/arm-io/usb-complex/"] == 0x239000000
    assert got["/arm-io/usb-complex/usb-device/"] == 0x239100000, \
        hex(got["/arm-io/usb-complex/usb-device/"])
    print("selfcheck ok")


def main():
    if sys.argv[1] == "--selfcheck":
        return selfcheck()
    doc = json.load(open(sys.argv[1]))["device-tree"]
    want = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None

    for path, node, base, regs in resolve(doc):
        if want and not want.search(path):
            continue
        compat = node.get("compatible")
        if not regs and not compat:
            continue
        print(path)
        if compat:
            print(f"    compatible: {compat}")
        for addr, size in regs:
            print(f"    reg: {addr:#x} size {size:#x}  abs {addr + base:#x}")
        for key in ("interrupts", "interrupt-parent", "clock-gates",
                    "function-clock_gate", "AAPL,phandle", "device_type"):
            if key in node:
                print(f"    {key}: {str(node[key])[:120]}")
        print()


if __name__ == "__main__":
    main()
