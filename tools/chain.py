#!/usr/bin/env python3
"""Drive the whole pre-boot chain, surviving the board dropping off USB.

Two things make this harder than calling laikadbg.py a few times:

Opening the board calls set_configuration(), which re-enumerates it and drops
whatever downstream connection it had. The first poll after any invocation
therefore reports device_type "none". Running "exploit" as a one-shot command
fires it at nothing, returns 0, and leaves pwned false, which reads as success.

The board also re-enumerates by itself after some stages, which kills the
session mid-poll with ENODEV. So rather than a fixed sequence, look at the
stage the phone is actually in and pick the one action that moves it forward.
Reopen the board whenever it vanishes and look again. Every action is chosen
from observed state, so resuming is safe and this can be re-run at any point.

Stages: dfu -> pwned-dfu -> laika-dfu -> pongo. "recovery" is a dead end,
it means l41ka's iBoot patchfinder did not install laikaDFU.

Usage:
    chain.py pongo              get to a PongoOS shell
    chain.py boot <image>       upload an image and run bootm
"""

import struct
import sys
import time
from pathlib import Path

L41KA = Path(__file__).resolve().parent.parent / "build" / "l41ka" / "scripts"
sys.path.insert(0, str(L41KA))

import laikadbg as L  # noqa: E402

TIMEOUT_MS = 60000
GONE = (OSError, L.ProtocolError, RuntimeError)


def open_board():
    """Wait for the board to come back after it re-enumerates."""
    for attempt in range(20):
        try:
            return L.LaikaClient()
        except Exception:  # noqa: BLE001
            if attempt == 0:
                print("  waiting for the board to come back")
            time.sleep(2.0)
    raise SystemExit("board never came back")


def reset_board():
    """Reset the board before trusting anything it says about the phone.

    The board caches the last device state it saw and keeps reporting it after
    the phone has moved on. That has reported a PongoOS shell for a phone
    sitting in DFU, three polls in a row, which makes the whole run a no-op
    that then claims success and costs an attempt to notice. Resetting first is
    the only thing observed to clear it.
    """
    print("  resetting the board")
    client = open_board()
    try:
        client.request(L.PI_RESET_PI, timeout_ms=TIMEOUT_MS)
    except GONE:
        pass  # it goes away as it resets, which is the point
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass
    time.sleep(3.0)


def stage(client):
    state = L.connection_from_payload(
        client.request(L.PI_POLL_FOR_DEVICE, timeout_ms=TIMEOUT_MS))
    return state["device_type"] if state["open"] else "none"


def info(client):
    return L.decode_device_info(
        client.request(L.DEVICE_DFU_INFO, timeout_ms=TIMEOUT_MS))


def to_pongo(deadline):
    """Walk the phone to a PongoOS shell, whatever stage it starts from.

    Hold one session open across iterations. Opening a LaikaClient calls
    set_configuration(), which re-enumerates the board and drops whatever
    downstream link it had, so reopening on every poll is not free: do that
    during the exploit and the phone gives up and boots its own iBoot into
    recovery, which is a dead end. Only reopen when the board actually goes
    away.
    """
    reset_board()

    seen = None
    recovery_strikes = 0
    pongo_strikes = 0
    client = open_board()
    try:
        while time.monotonic() < deadline:
            try:
                here = stage(client)
                if here != seen:
                    print(f"  stage: {here}")
                    seen = here

                if here != "recovery":
                    recovery_strikes = 0
                if here != "pongo":
                    pongo_strikes = 0

                if here == "pongo":
                    # Do not trust one reading here either. This has reported
                    # pongo on the very first poll for a phone sitting in DFU,
                    # which makes the whole run a no-op that then reports
                    # success. Same stale state problem as recovery below.
                    pongo_strikes += 1
                    if pongo_strikes < 3:
                        time.sleep(1.0)
                        seen = None
                        continue
                    return
                if here == "none":
                    time.sleep(2.0)
                elif here == "dfu":
                    d = info(client)
                    print(f"  {d['product_type']} cpid {d['cpid']} bdid {d['bdid']}")
                    print("exploit")
                    client.request(L.DEVICE_DFU_EXPLOIT, timeout_ms=TIMEOUT_MS)
                elif here == "pwned-dfu":
                    print("setup-iboot")
                    client.request(
                        L.DEVICE_PWNED_DFU_SEND_EMBEDDED_IBOOT_PATCHFINDER_AND_BOOT,
                        timeout_ms=TIMEOUT_MS)
                elif here == "laika-dfu":
                    print("load-pongo")
                    client.request(L.DEVICE_LAIKADFU_SEND_EMBEDDED_PONGOOS,
                                   timeout_ms=TIMEOUT_MS)
                elif here == "recovery":
                    # Do not trust one reading. A poll taken just after the
                    # board re-enumerates reports stale state, and this has
                    # said "recovery" for a phone that was sitting in DFU.
                    # Only believe it if it keeps saying so.
                    recovery_strikes += 1
                    if recovery_strikes < 5:
                        time.sleep(2.0)
                        seen = None
                        continue
                    raise SystemExit(
                        "stuck in recovery: the iBoot patchfinder did not "
                        "install laikaDFU, so there is no way forward from here")
                else:
                    raise SystemExit(f"unexpected stage {here}")
            except GONE as error:
                print(f"  board went away ({error}), reopening")
                try:
                    client.close()
                except Exception:  # noqa: BLE001
                    pass
                client = open_board()
                seen = None
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass
    raise SystemExit("gave up before reaching pongo")


def boot(image, deadline):
    while time.monotonic() < deadline:
        client = open_board()
        try:
            here = stage(client)
            if here != "pongo":
                print(f"  stage: {here}, waiting for pongo")
                time.sleep(2.0)
                continue

            print(f"upload {image} ({image.stat().st_size} bytes)")
            L.stream_upload(client, image, L.DEVICE_PONGO_BEGIN_UPLOAD_FILE,
                            L.DEVICE_PONGO_FINISH_UPLOAD_FILE, 1,
                            128 * 1024 * 1024, TIMEOUT_MS)

            # A stalled pipe here is the expected result, not an error: PongoOS
            # takes the command and jumps, so it stops servicing USB mid
            # transfer. Watch the phone screen from this point, the console is
            # the framebuffer m1n1 inherits from iBoot.
            print("bootm")
            try:
                client.request(L.DEVICE_PONGO_SEND_COMMAND,
                               struct.pack("<I", TIMEOUT_MS) + b"bootm",
                               TIMEOUT_MS)
            except GONE as error:
                print(f"  usb went away: {error}")
                print("  that is what a successful handoff looks like")
                return
            L.drain_pongo_output(client, TIMEOUT_MS)
            return
        except GONE as error:
            print(f"  board went away ({error}), reopening")
        finally:
            try:
                client.close()
            except Exception:  # noqa: BLE001
                pass
    raise SystemExit("gave up before booting")


def run(commands, deadline):
    """Run shell commands in PongoOS and print what they say.

    This is the only place where a working USB gadget on this SoC can be
    questioned directly: PongoOS brings its own up, which is how the board
    talks to it at all. "synopsys" dumps the DWC2 registers and "peek" reads
    any 32 bit register, so the USB complex, the PHY and the core can all be
    read out of a setup that works, rather than guessed at one DFU cycle at a
    time.
    """
    client = open_board()
    try:
        for command in commands:
            print(f"\n=== {command} ===")
            client.request(L.DEVICE_PONGO_SEND_COMMAND,
                           struct.pack("<I", TIMEOUT_MS) + command.encode(),
                           TIMEOUT_MS)
            L.drain_pongo_output(client, TIMEOUT_MS)
    finally:
        try:
            client.close()
        except Exception:  # noqa: BLE001
            pass


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "pongo"
    deadline = time.monotonic() + 600

    if what == "pongo":
        to_pongo(deadline)
        print("pongo shell is up")
    elif what == "boot":
        to_pongo(deadline)
        boot(Path(sys.argv[2]), deadline)
    elif what == "run":
        to_pongo(deadline)
        run(sys.argv[2:], deadline)
    else:
        raise SystemExit(
            f"usage: {sys.argv[0]} pongo | boot <image> | run <command>...")


if __name__ == "__main__":
    main()
