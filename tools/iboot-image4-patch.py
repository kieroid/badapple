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
REG_MASK = 0xFFFFFFE0


def words(buf):
    return struct.unpack(f"<{len(buf) // 4}I", buf[: len(buf) // 4 * 4])


def find_callback(w):
    for i in range(len(w) - 1):
        if (w[i] & REG_MASK, w[i + 1] & REG_MASK) != (EKEY_MOVZ, EKEY_MOVK):
            continue
        for j in range(i, max(i - 0x400, -1), -1):
            if w[j] == PACIBSP:
                yield j * 4, i * 4
                break


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} <iboot.raw> <patched.raw>")

    data = bytearray(open(sys.argv[1], "rb").read())
    # Keyed by function, because the callback compares against EKEY more than
    # once and every one of those walks back to the same entry.
    hits = {}
    for start, anchor in find_callback(words(data)):
        hits.setdefault(start, anchor)
    if len(hits) != 1:
        sys.exit(f"expected one function with an EKEY anchor, found "
                 f"{len(hits)}: {[hex(o) for o in sorted(hits)]}")

    (start, anchor), = hits.items()
    struct.pack_into("<I", data, start, MOV_W0_0)
    struct.pack_into("<I", data, start + 4, RET)
    open(sys.argv[2], "wb").write(bytes(data))
    print(f"image4_validate_property_callback at {start:#x} (EKEY at {anchor:#x}), returns 0 now")


if __name__ == "__main__":
    main()
