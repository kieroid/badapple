# Notes

Detail that does not belong in the README. Register values, gotchas, and the
things that turned out to be wrong.

## Reading the ADT

    ipsw extract --pattern 'DeviceTree.*' -o work <ipsw>
    ipsw dtree --json DeviceTree.d331pap.im4p > refs/iosdt/d331pap.json
    python3 tools/iosdt.py refs/iosdt/d331pap.json uart

Every `reg` in that tree is an offset from its parent's base, and it nests.
`/arm-io` sets the window from its `ranges`, so `usb-complex` at `0x39000000`
is really `0x239000000`, and `usb-device` at `0x100000` under it is
`0x239100000`, not `0x200100000`. `tools/iosdt.py` accumulates the base down
the tree. Getting it wrong is silent, so:

    python3 tools/iosdt.py --selfcheck

## CPU affinity

CPU `reg` must equal `MPIDR_EL1 & 0xFFFFFF`. The ADT's `reg` is not that
number: it reports 0 for cpu0 and 256 for cpu4, omitting the Aff2 bit. The boot
core reports MPIDR `0x80010100`, so cluster 1 core 0 is `0x10100`.

With the ADT values taken at face value, m1n1 prunes all six CPU nodes
including the one it is running on, and Linux gets a tree with no CPUs.

## The two consoles

- **Output**: the framebuffer m1n1 hands over from iBoot, as
  `apple,simple-framebuffer` plus `console=tty0`. No display driver, so the
  picture is frozen as handed over.
- **Input**: a USB CDC-ACM gadget the initramfs brings up. This is why the tree
  needs the USB complex, the PHY, dwc2 and the PMGR power domains.

UART0 exists and `earlycon` uses it, but reading it needs a serial cable on the
Lightning pins.

The framebuffer node needs `power-domains`. Without them nothing claims
`ps_disp0_be`, `ps_mipi_dsi`, `ps_disp0_hilo` or `ps_disp0_ppp` and
`apple-pmgr-pwrstate` powers them down as unused, which looks like the display
dying mid-boot.

## m1n1 patches

Six, in `patches/`. Three were T8020 bugs, three were about seeing anything.

1. **`0001` USB complex registers.** `usb-complex,t8020` fell through to the
   `t8011` case and got its DMA remap offsets. See below.
2. **`0005` A12 MIDR parts.** `midr.h` defined `MIDR_PART_T8020_VORTEX` and
   `_TEMPSET` but nothing referenced them, so the lookup fell back to
   `features_unknown` with `apple_sysregs_unlocked` false, sending an A12 down
   an M4-only path reading `CTRR_M4_LWR_EL2` from EL1.
3. **`0002` SError handling.** An asynchronous SError fires at the top-of-RAM
   boundary, `L2C_ERR_ADR` `0x8ec78c000`, every boot. m1n1's existing clear is
   `reg_mask(..., ACCESS_FAULT | PIO_FAULT, 0)`, a write of zeroes, but the
   register is write-1-to-clear: `ENABLE_W1C`, bit 56, is set. Writing ones to
   the fault bits, `RECURSIVE_FAULT` included, works. Root cause still unknown.

   Masking DAIF in the handler does nothing: ERET restores SPSR.

`0003`, `0004` and `0006` exist because the framebuffer is the only console
without a serial cable, and a `RELEASE` build defines `FB_SILENT_MODE`, which
silences it exactly when a payload is present.

## USB complex offsets

The ADT lists `/arm-io/usb-complex usb_widget_with_mask` as three
(offset, value, mask) triplets at 0x28, 0x38 and 0x48, value 0x88, mask
0x030000ff.

Confirmed against hardware by peeking a running PongoOS:

    0x239000000: 1          complex enable
    0x239000048: 3000088    device remap

PongoOS's `t8030` branch gives `0x8015` and `0x8020` a shared case: enable at
offset 0, device remap at 0x48. Only the 0x48 write matters.

Wrong turn worth recording: an earlier attempt rewrote this to the S5L8960X
layout, 0x108 at 0x1c and 0x5c, from reading PongoOS's `iOS15` branch where
`0x8020` falls through to `default`. That is the wrong branch. l41ka pins
`t8030`; check the submodule commit before trusting anything in that tree.

## A working gadget on this SoC

From `make usbregs` against a parked m1n1 the PC has enumerated:

    GAHBCFG  0x0000002e      DCTL  0x00000000      DCFG  0x00000074
    DSTS     0x002c7e00      GSNPSID 0x4f54300a
    GUSBCFG  0x00001408      GOTGCTL 0x000d0000

`DCTL` 0 means the soft disconnect is clear, `DCFG` says the host assigned
address 7, `DSTS` decodes to high speed, core revision 3.00a.

PongoOS over the jig agrees on `GUSBCFG` and the complex. Its own `DSTS` shows
full speed, because the RP2350 is a bit-banged PIO host.

### AUSB SIG bits are shifted on T8020

PongoOS reads SIG `0x00d10003` while connected. That is m1n1's documented
`0xd103` with every upper bit shifted up by 8:

| | bits set |
|---|---|
| m1n1 `0xd103` (cable connected) | 0, 1, **8, 12, 14, 15** |
| T8020 `0x00d10003` | 0, 1, **16, 20, 22, 23** |

So `AUSB_SIG_CABLE_CONNECTED` is `BIT(16)`, not `BIT(8)`, and
`AUSB_SIG_HOST` is `7 << 20`, not `7 << 12`. The Linux `ausb` driver tests bit
8 and never sees a cable.

## The cable move

The A11 port never hits this. checkm8 runs from the PC, so the phone is on the
PC for the whole boot. usbliter8 needs the RP2350 as host, so the phone is on
the jig when dwc2 probes, binds its pullup there, and moving the cable
afterwards does nothing: the host sees the attach and the high speed chirp,
which are the PHY, then times out on every control transfer with
`device descriptor read/64, error -110`.

m1n1 hit exactly this. Fixed by not moving the cable: boot m1n1 from the PC
over PongoOS and its proxy enumerates first try.

Two things learned the hard way:

- A diagnostic loop polling the gadget every 100ms was enough on its own to
  stop enumeration. A host will not wait that long for EP0.
- m1n1's USB is polled from `uartproxy_run()`, not interrupt driven, so
  anything keeping the CPU out of that loop looks exactly like broken USB.

## Linux dwc2 writes never land

Writing a known value to four registers and reading each back, from
`dwc2_hsotg_pullup()`:

    wtest GINTMSK  00000000 -> 00000000 (wrote 00003000)
    wtest DIEPMSK  00000000 -> 00000000 (wrote 0000000d)
    wtest GAHBCFG  00000000 -> 00000000 (wrote 0000002e)
    wtest DCTL     00000002 -> 00000002 (wrote 00000000)

Not one takes. `GINTMSK` and `DIEPMSK` read 0 even though
`dwc2_hsotg_core_init_disconnected()` wrote them moments earlier, so nothing
this driver has ever written to this controller has had any effect.

Reads work and match m1n1 on the same silicon. Probe reads `GHWCFG` well enough
to report 9 EPs and 2056 SPRAM entries. `dwc2_writel()` is a plain `writel()`
through the same `hsotg->regs` pointer `dwc2_readl()` reads through.

That one mechanism explains every earlier dead end, because all of them were
writes into a block that was not listening.

### Ruled out, with the evidence

| Theory | Killed by |
|---|---|
| Hardware, cable, hub, port, host | PongoOS enumerates on this PC as `05ac:4141`, same cable |
| Complex enable bit not set | Reads back `0x1`, matches PongoOS |
| DMA remap offsets wrong | `0x48` reads `0x3000088`, matches PongoOS |
| `GUSBCFG` PHYIF16 misdetected | Already `0x1408`, which is what PongoOS writes |
| SError storm starving the poll loop | Handler rate limited, screen stays clean |
| dwc2 core soft reset wedging the core | Skipped for `apple,dwc2`, no change |
| USB power domains gated off | `apple,always-on` on all three; pmgr warns about `debug` and `debug_usb` only |
| High speed only failure | m1n1 enumerates at high speed, `DSTS` `0x002c7e00` |
| Non-coherent DMA needing cache maintenance | Added it, no change |

### Next step

Find out why `writel()` to `hsotg->regs + offset` is dropped while `readl()`
through the identical pointer works. Boot with `no_hash_pointers` so the
`regs=` line prints a real address instead of a hashed one, and compare against
`0x239100000`, which m1n1 peeks successfully.

Then, once it enumerates, configure the host end as root:

    sudo ip link set <iface> up
    sudo ip addr add 172.16.42.2/24 dev <iface>
    telnet 172.16.42.1

The phone is 172.16.42.1 and runs a DHCP server.

## Corrections

Things this repo previously asserted that are wrong.

- **"Any repacked initramfs panics the kernel."** No. Generalised from two
  failures of a flaky chain. `init/initramfs.c` handles concatenated archives,
  the base archive is 512 byte aligned, and the repacked one boots. The real
  bug was in the injected script: at `rdinit=` time busybox has not run
  `--install -s`, so neither `/bin/sh` nor `sleep` exists.
- **"The cable move never worked on this host."** It had already worked,
  enumerating as `1209:316d`. A workflow was deleted on that claim and had to
  be restored.
- **`05ac:12a8` is our gadget.** It is a different phone. Ours is `05ac:4142`.

## Jig reliability

The board caches the last device state it saw and keeps reporting it after the
phone has moved on, which is worse than failing because the run looks like it
succeeded. It has reported a PongoOS shell, three polls running, for a phone in
DFU. `chain.py` resets the board before trusting it, which is the only thing
observed to clear it.

Do not run the stages as separate `laikadbg.py` commands. Opening the board
calls `set_configuration()`, which re-enumerates it and drops the downstream
link, so a one-shot `exploit` fires at nothing, returns 0, and leaves `pwned`
false. `chain.py` holds one session open across all stages.

## RP2350 firmware

l41ka ships no binary in its git tree, only in releases, and the release is
marked prerelease so `releases/latest` returns 404. One asset:

    https://github.com/0cyn/l41ka/releases/download/0.1.0/l41ka-waveshare-rp2350-usb-a.zip

Unpacks to `l41ka.uf2`, `l41ka.elf`, `l41ka.bin`, `l41ka.hex`, a disassembly
and a link map. Lives in `build/l41ka-fw/` by hand. To flash:

    picotool load -t elf build/l41ka-fw/l41ka.elf -x -f

Or hold BOOTSEL and copy `l41ka.uf2` onto the mass storage device. Coming from
older firmware, `laikadbg.py bootsel` or `picotool reboot -f -u` first.

The 0.1.0 firmware and a current clone are the same version in practice: the
only commit after the tag is a README screenshot.

## Checking the phone is pwned

    cat /sys/bus/usb/devices/*/serial

    CPID:8020 CPRV:11 CPFM:03 SCEP:01 BDID:1A ECID:... IBFL:3C
    SRTG:[iBoot-3865.0.0.4.7] PWND:[usbliter8]

`CPID:8020` is the chip id, `BDID:1A` the D331pAP board id.

## Device stages

`dfu -> pwned-dfu -> laika-dfu -> pongo`. `recovery` is a dead end: it means
l41ka's iBoot patchfinder did not install laikaDFU.

## Why no hypervisor target

m1n1's hypervisor would trap the guest's UART writes and give Linux's early
console for free, but it needs EL2 and PongoOS hands off at EL1. See its own
comment in `src/kernel/entry.c`, "We're in EL1 here", with elevation only on
SoCs that have EL3. The ADT's `has-virtualization = 1` is about the silicon,
not the exception level we are handed.
