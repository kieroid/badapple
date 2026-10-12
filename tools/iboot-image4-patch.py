#!/usr/bin/env python3
# Neuter image4_validate_property_callback in a raw iBoot image, so iBSS will
# load a component whose hash Apple never signed.
#
# The component still needs an IM4M in its container. iBoot parses the manifest
# before it ever asks about a single property, and with no manifest at all it
# gives up earlier than this, somewhere that extraction never runs. Any IM4M
# will do once this patch is in, the per-property checks that would reject a
# borrowed one are exactly what stops running.
#
# The callback is found the way hBootPatcher finds it, by the "EKEY" literal it
# compares against, built inline as a movz/movk pair. hBootPatcher itself walks
# forward from there to a `ret`, which fails on this firmware because iBoot is
# PAC signed now and returns through `retab`. Walking back to the `pacibsp` that
# opens the function is both simpler and version independent.

import sys
import struct

PACIBSP = 0xD503237F
MOV_W0_0 = 0x52800000
RET = 0xD65F03C0

# movz wN, #0x4559 / movk wN, #0x454b, lsl #16, ignoring which register
EKEY_MOVZ, EKEY_MOVK = 0x5288AB20, 0x72A8A960
# the same pair in the other order, which older compilers emit
EKEY_MOVZ_ALT, EKEY_MOVK_ALT = 0x52A8A960, 0x7288AB20
REG_MASK = 0xFFFFFFE0


def words(buf):
    return struct.unpack(f"<{len(buf) // 4}I", buf[: len(buf) // 4 * 4])


def find_callback(w):
    for i in range(len(w) - 1):
        a, b = w[i] & REG_MASK, w[i + 1] & REG_MASK
        if (a, b) not in ((EKEY_MOVZ, EKEY_MOVK), (EKEY_MOVZ_ALT, EKEY_MOVK_ALT)):
            continue
        for j in range(i, max(i - 0x400, -1), -1):
            if w[j] == PACIBSP:
                yield j * 4, i * 4
                break


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <iboot.raw> <patched.raw>")

    data = bytearray(open(sys.argv[1], "rb").read())
    hits = {start: anchor for start, anchor in find_callback(words(data))}
    if not hits:
        sys.exit("no EKEY anchor found, not an iBoot image for this patch")
    if len(hits) > 1:
        found = ", ".join(hex(o) for o in sorted(hits))
        sys.exit(f"EKEY anchor resolved to several functions: {found}")

    start, anchor = next(iter(hits.items()))
    struct.pack_into("<I", data, start, MOV_W0_0)
    struct.pack_into("<I", data, start + 4, RET)
    open(sys.argv[2], "wb").write(bytes(data))
    print(f"image4_validate_property_callback at {start:#x} (EKEY at {anchor:#x}), returns 0 now")


if __name__ == "__main__":
    main()
