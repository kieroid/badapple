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

Seven, in `patches/`. Four were T8020 bugs, three were about seeing anything.

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

4. **`0007` dwc2 DMA coherency and PHY width.** Two separate T8020 bugs in one
   file. There is no DART in the USB path, only the remap widget, which is not
   an IOMMU and does not snoop, so with caches on the CPU reads stale SETUP
   packets out of DRAM and never answers the host. PongoOS invalidates before
   reading and cleans before handing a buffer over, and m1n1 now does the same.
   Separately, `usb_dwc2_init()` decided 16 bit UTMI+ from the `GUSBCFG` it
   read, but `usb_bringup` has just reset the PHY, so `PHYIF16` reads clear and
   it fell through to the ULPI case with an 8 bit turnaround. Both faults look
   identical from the host: attach, high speed chirp, then
   `device descriptor read/64, error -110`.

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

Dumped from a parked m1n1 that the PC had enumerated, back when the Linux
gadget did not work and the two needed comparing:

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

## Linux dwc2 writes never land, solved

Every write dwc2 made was dropped while every read worked and agreed with
m1n1 on the same silicon:

    wtest GINTMSK  00000000 -> 00000000 (wrote 00003000)
    wtest DIEPMSK  00000000 -> 00000000 (wrote 0000000d)
    wtest GAHBCFG  00000000 -> 00000000 (wrote 0000002e)
    wtest DCTL     00000002 -> 00000002 (wrote 00000000)

The cause was a missing `nonposted-mmio` on the `bus@239000000` node.
`of_mmio_is_nonposted()` only looks at the node and its immediate parent, so
the one on `soc` does not reach anything a bus further down, and the PHY and
dwc2 were mapped with `ioremap()` rather than `ioremap_np()`. Upstream repeats
the property on this same bus for every A series SoC, `s5l8960x.dtsi` included.
With it added, the same test reads:

    wtest GINTMSK  d08c3cc4 -> 00003000 (wrote 00003000)
    wtest DIEPMSK  0000020f -> 0000000d (wrote 0000000d)
    wtest GAHBCFG  00000023 -> 0000002e (wrote 0000002e)

A plain `ioremap()` of the same block, taken from the ausb driver, still reads
`GRXFSIZ 0x840 -> 0x840` after writing it, which is the same bug seen from the
other side.

This mechanism explains every earlier dead end at once, because all of them
were writes into a block that was not listening.

### Ruled out along the way, with the evidence

| Theory | Killed by |
|---|---|
| Hardware, cable, hub, port, host | PongoOS enumerates on this PC as `05ac:4141`, same cable |
| Complex enable bit not set | Reads back `0x1`, matches PongoOS |
| DMA remap offsets wrong | `0x48` reads `0x3000088`, matches PongoOS |
| `GUSBCFG` PHYIF16 misdetected | Already `0x1408`, which is what PongoOS writes |
| SError storm starving the poll loop | Handler rate limited, screen stays clean |
| dwc2 core soft reset wedging the core | Skipped for `apple,dwc2`, no change |
| USB power domains gated off | All three read `0x000000ff`, on and settled |
| Core clamped by `PCGCTL` | Reads `0x00000000`, nothing gated |
| High speed only failure | m1n1 enumerates at high speed, `DSTS` `0x002c7e00` |
| Non-coherent DMA needing cache maintenance | Added it, no change |
| dwc2's own mapping at fault | A second independent `ioremap()` behaved identically |

## Forced VBUS detection is needed after all

`patches/linux-0001` used to clear `AUSB_SIG_VBUSDET_FORCE_EN`, on the reading
that the real detector was being pinned to a value sampled while the PHY was
still settling. That reading came from runs where no dwc2 write landed, so it
proved nothing. With the bit set the host sees the attach and the high speed
chirp. The PHY then reads `SIG 0x01c10003`, which is m1n1's documented
`0x1c103` shifted by the usual 8, and m1n1's own table calls that value "USB
proxy active".

The same patch used to clear `AUSB_SIG_HOST`. That mask is `7 << 12`, which on
T8020 is three bits that are not the role field, and m1n1 never touches it.
Dropped.

## No AIC interrupt is ever delivered, solved

genpd powered the interrupt controller off.

The AIC driver is registered with `IRQCHIP_DECLARE`, which runs long before the
driver model exists. It never creates a device, so nothing ever becomes a
consumer of the `ps_aic` power domain. Late in boot, "PM: genpd: Disabling
unused power domains" finds a domain with no users and switches it off. From
that moment the controller is dead: no interrupt is delivered, and any read of
an AIC register hangs the core outright, with no oops and no further output.

The boot still looks healthy, because on Apple SoCs the architected timer is a
core local FIQ that does not go through AIC at all. Userspace runs, the DHCP
daemon starts, the debug shell comes up, and the only thing missing is every
device interrupt in the machine.

Upstream marks this domain always-on as a core device. `dts/t8020-pmgr.dtsi` is
generated by `make pmgr` from the iOS DeviceTree, which has no equivalent
property, so the annotation was missing. The fix is three lines in
`dts/t8020.dtsi`:

    &ps_aic {
        apple,always-on;
    };

With that in place the gadget enumerates on the first try:

    Bus 003 Device 116: ID 05ac:4142 Apple, Inc. HoolockLinux USB Device

and the host picks up `172.16.42.2/24` by DHCP from the phone.

Any other domain whose consumer is not a normal platform driver will have the
same problem. The USB domains in `dts/t8020.dtsi` were already pinned by hand
for a different reason, which is why they were not also hit by this.

### How it presented

The gadget got as far as the host seeing it attach, then every control transfer
timed out with `device descriptor read/64, error -110`, because nothing
serviced the bus.

A counter in `dwc2_hsotg_irq()` stayed at zero while the controller sat with
work pending:

    apple dbg: irqs=0 GINTSTS=0x0408bc38 GINTMSK=0xd08c3cc4 DSTS=0x002bfe00

`GINTSTS & GINTMSK` was `0x00083c00`, which includes USBRst and EnumDone. A
second counter in `aic_handle_irq()` also stayed at zero, which ruled out
anything specific to hwirq 451 and pointed at the controller as a whole.

The decisive clue was accidental. Adding a read of any AIC register to a
delayed work item hung the boot dead at the line before it. Those same reads
work during init. A block that answers at init and hangs the bus later is a
block that has been powered down in between, and the only thing that powers
anything down in between is genpd.

Established about AIC, from the hardware:

- `AIC_WHOAMI` reads 4 on the boot CPU. The A12 starts on cluster 1 core 0,
  which is logical CPU 0 to the kernel, so the two numbering schemes disagree.
  Upstream only warns about this and leaves a note that a mapping table would
  be needed if it ever happened. `patches/linux-0003` adds one.
- IRQ 451 is unmasked: `mask_set` reads `0xfffffff7`, bit 3 clear.
- `nr_irq` is 512 while the driver hardcodes `max_irq` 1024. Sizing the
  register banks from `nr_irq` instead moves `mask_set` to `0x3880`, where it
  reads `0x00000000`, so 1024 is correct and PongoOS agrees: it uses `0x4100`
  and `0x4180` directly.
- `AIC_CONFIG` bit 0 is AIC's global enable. PongoOS sets `0xe0000001` in its
  `interrupt_init()`. Setting it from Linux too changes nothing.
- `TARGET_CPU` has been tried as `BIT(0)`, as `BIT(whoami)`, and left as iBoot
  set it, which is what PongoOS does. None deliver an interrupt.
- Reading AIC at `0x4200`, where the driver's layout puts `HW_STATE`, raises a
  synchronous external abort. So does reading `TARGET_CPU` for some hwirqs.

PongoOS does take AIC interrupts on this SoC after the same exploit, including
for USB: `usb_irq = otg_irq` with `task_register_preempt_irq()`. So delivery is
possible here and something in the handover or in the kernel's setup is in the
way.

### Dead ends, recorded so they are not retried

The exception level was the leading theory and it was wrong. PongoOS never
writes `HCR_EL2` at all, and erets straight from EL3 to EL1 with
`SCR_EL3 = 0x430`. m1n1 only writes `HCR_EL2` under `if (in_el2())`, which is
false on this path. The running kernel confirms it: `ISR_EL1=0x0`,
`DAIF=0x0`, `CurrentEL=1`.

`AIC_CONFIG` bit 0 was also suspected, since PongoOS sets `0xe0000001` in its
`interrupt_init()`. Writing it from Linux was tried, kept for a while, and then
removed again once interrupts worked: a boot with the write compiled out
enumerates exactly the same. It was never needed.

`TARGET_CPU` was tried as `BIT(0)`, as `BIT(whoami)`, and left alone. Leaving
it alone is correct and is what PongoOS does, but none of the three was ever
the problem.

### Reaching the shell

The host takes `172.16.42.2` by DHCP from the phone, which is `172.16.42.1`
and runs `telnetd` on port 23 plus a getty on `/dev/ttyGS0`. Nothing needs
configuring by hand:

    telnet 172.16.42.1

The ACM getty blocks on a read before it prints anything, by design, so an
idle `/dev/ttyACM*` that says nothing is not a fault. Note there are two ACM
devices: the jig is `4141:4c41` and the phone is `05ac:4142`.

## The gadget prints are load bearing

The six `dev_info` calls `linux-0002` adds to `dwc2_hsotg_udc_start()` and
`dwc2_hsotg_pullup()` read like leftover bring-up debugging. They were removed
on exactly that reasoning, as a tidy-up, with no other change in the build. The
gadget stopped enumerating:

    dwc2 239100000.usbdev: new device is high-speed

and then nothing, with the host reporting `device descriptor read/64, error
-110` and `device not accepting address N, error -62`. Putting them back, with
nothing else changed, enumerates again on the next boot.

The console is a 155x168 simpledrm framebuffer, so each of those prints costs
milliseconds, and they are the only thing separating `dwc2_force_mode()`,
`dwc2_hsotg_core_connect()` and the manual `DCTL_SFTDISCON` clear. Upstream's
own waits are evidently not enough for this core.

So they are delays wearing a print's clothing. The real fix is to find which
gap matters and put an explicit delay there, which costs a DFU cycle per guess.
Until someone does that, leave all six alone.

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
