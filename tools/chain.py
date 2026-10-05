#!/usr/bin/env python3
"""Drive the whole pre-boot chain in one l41ka USB session.

Why this exists: laikadbg.py runs one command per process, and opening the
board calls set_configuration(), which re-enumerates it and drops whatever
downstream connection it had. The first poll after any invocation therefore
always reports device_type "none". Running "exploit" as a one-shot command
fires it at nothing, which looks like success (rc 0) and leaves pwned false.

So open the board once, wait for the phone to actually be there, and only then
walk the stages. Each stage waits for the link to come back, because the phone
re-enumerates as it moves between dfu, pwned-dfu, laika-dfu and pongo.

Usage:
    chain.py pongo              exploit, patch iBoot, load PongoOS
    chain.py boot <blob>        upload a blob and run bootm
    chain.py pongo <blob>       both, in one session
"""

import struct
import sys
import time
from pathlib import Path

L41KA = Path(__file__).resolve().parent.parent / "build" / "l41ka" / "scripts"
sys.path.insert(0, str(L41KA))

import laikadbg as L  # noqa: E402

TIMEOUT_MS = 60000


def poll(client, want, tries=20, delay=2.0):
    """Wait for an open downstream link whose type is one of want."""
    if isinstance(want, str):
        want = (want,)
    last = None
    for _ in range(tries):
        last = L.connection_from_payload(
            client.request(L.PI_POLL_FOR_DEVICE, timeout_ms=TIMEOUT_MS))
        if last["open"] and last["device_type"] in want:
            print(f"  link: {last['device_type']}")
            return last
        time.sleep(delay)
    raise SystemExit(f"timed out waiting for {'/'.join(want)}, last state {last}")


def info(client):
    return L.decode_device_info(
        client.request(L.DEVICE_DFU_INFO, timeout_ms=TIMEOUT_MS))


def do_pongo(client):
    poll(client, ("dfu", "pwned-dfu"))
    d = info(client)
    print(f"  {d['product_type']} cpid {d['cpid']} bdid {d['bdid']} pwned {d['pwned']}")

    if not d["pwned"]:
        print("exploit")
        client.request(L.DEVICE_DFU_EXPLOIT, timeout_ms=TIMEOUT_MS)
        poll(client, "pwned-dfu")
        d = info(client)
        if not d["pwned"]:
            raise SystemExit("exploit ran but pwned is still false")
    print(f"  pwned: {d['pwned']}")

    print("setup-iboot")
    client.request(L.DEVICE_PWNED_DFU_SEND_EMBEDDED_IBOOT_PATCHFINDER_AND_BOOT,
                   timeout_ms=TIMEOUT_MS)
    poll(client, ("laika-dfu", "recovery", "pwned-dfu"))

    print("load-pongo")
    client.request(L.DEVICE_LAIKADFU_SEND_EMBEDDED_PONGOOS, timeout_ms=TIMEOUT_MS)
    poll(client, "pongo")


def do_boot(client, blob):
    poll(client, "pongo")
    print(f"upload {blob} ({blob.stat().st_size} bytes)")
    L.stream_upload(client, blob, L.DEVICE_PONGO_BEGIN_UPLOAD_FILE,
                    L.DEVICE_PONGO_FINISH_UPLOAD_FILE, 1, 128 * 1024 * 1024,
                    TIMEOUT_MS)
    print("bootm")
    client.request(L.DEVICE_PONGO_SEND_COMMAND,
                   struct.pack("<I", TIMEOUT_MS) + b"bootm", TIMEOUT_MS)
    L.drain_pongo_output(client, TIMEOUT_MS)


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "pongo"
    blob = Path(sys.argv[2]) if len(sys.argv) > 2 else None

    with L.LaikaClient() as client:
        if stage == "pongo":
            do_pongo(client)
        if blob is not None:
            do_boot(client, blob)


if __name__ == "__main__":
    main()
