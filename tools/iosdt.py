#!/usr/bin/env python3
"""Read an Apple iOS DeviceTree dumped to JSON by "ipsw dtree --json" and print
the parts that matter when writing a Linux DTSI for the same SoC.

The iOS tree stores reg as a flat list of 64 bit address/size pairs that are
relative to the parent node. For nodes under /arm-io the parent base comes from
the arm-io ranges property, so an absolute address is arm_io_base + offset.

Usage:
    iosdt.py d321ap.json                 list every node with reg and compatible
    iosdt.py d321ap.json uart            only nodes whose path matches "uart"
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
        if all(isinstance(v, int) for v in value):
            return list(zip(value[0::2], value[1::2]))
    if isinstance(value, dict):
        return [(value.get("addr", 0), value.get("size", 0))]
    return []


def main():
    doc = json.load(open(sys.argv[1]))["device-tree"]
    want = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None

    # arm-io ranges is a list of (child, parent, size) triplets. The parent
    # address of the first triplet is the base every peripheral offset adds to.
    arm_io_base = 0
    for path, node in walk(doc):
        if path == "/arm-io/":
            words = [a for pair in as_u64_pairs(node.get("ranges"))
                     for a in pair]
            if len(words) >= 2:
                arm_io_base = words[1]
            break

    print(f"# arm-io base: {arm_io_base:#x}\n")
    for path, node in walk(doc):
        if want and not want.search(path):
            continue
        regs = as_u64_pairs(node.get("reg"))
        compat = node.get("compatible")
        if not regs and not compat:
            continue
        print(path)
        if compat:
            print(f"    compatible: {compat}")
        for addr, size in regs:
            absolute = addr + arm_io_base if path.startswith("/arm-io/") else addr
            print(f"    reg: {addr:#x} size {size:#x}  abs {absolute:#x}")
        for key in ("interrupts", "interrupt-parent", "clock-gates",
                    "function-clock_gate", "AAPL,phandle", "device_type"):
            if key in node:
                print(f"    {key}: {str(node[key])[:120]}")
        print()


if __name__ == "__main__":
    main()
