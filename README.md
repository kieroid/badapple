# linux4a12

Mainline Linux bring-up for the Apple A12 (T8020), targeting the iPhone XS Max
(iPhone11,6, board D331pAP).

Goal for this stage is narrow on purpose: a console and an initramfs shell. No
display driver, no touch, no wifi, no cellular, no GPU. That is also as far as
the A10 and A11 ports have got, so it is the honest finish line.

## Why A12 is possible now

Upstream Linux has device trees for A7 through A11 only, because the boot chain
needed checkm8 and checkm8 stops at A11. The usbliter8 SecureROM exploit
(published June 2026) covers A12 and A13, which opens the same door on this
phone. Pre-boot code execution is already solved by other people, so the work
left here is the kernel side.

## Boot chain

    SecureROM
      -> usbliter8 (RP2350 board sends crafted USB setup packets in DFU)
      -> PWND DFU
      -> patched iBSS / iBEC
      -> PongoOS
      -> bootm
      -> m1n1
      -> Linux (16K pages) + t8020-d331p.dtb + initramfs

Nothing is written to the phone. Reboot returns it to iOS.

The pre-boot half is l41ka, which is the only thing that gets an A12 this far.
It runs usbliter8 from the RP2350 and embeds a PongoOS fork with T8020 support.
That matters in two ways:

- Upstream checkra1n PongoOS has no T8020 support, and Hoolock's prebuilt
  `Pongo.bin` covers A7 to A11 and T2 only.
- `bootm`, the command that chainloads m1n1, is not an upstream PongoOS
  command. Upstream has `bootl`, `bootx`, `bootr` and `bootux`. Hoolock added
  `bootm`, and 0cyn's `PongoOS` fork on the `t8030` branch has it too, which is
  the build l41ka ships. It boots whatever `/send` left in the loader buffer as
  a raw m1n1 image, so the appended payloads are m1n1's problem, not PongoOS's.

l41ka is not open source ("All Rights Reserved", contact the author for
licensing), so it stays a tool in `build/` and none of it is vendored here. Its
`scripts/laikadbg.py` is enough to drive the whole chain from the command line,
no GUI app needed, and `make pongo` wraps that.

Worth noting for later: the l41ka author vendored m1n1's DART driver into their
PongoOS fork, which suggests the USB DART on A12 needs more care than the
`dma-ranges` bypass this tree currently assumes.

## The two consoles

Worth being clear about, because it drives what the device tree has to contain:

- **Output** is the framebuffer m1n1 hands over from iBoot, as
  `apple,simple-framebuffer` plus `console=tty0`. No display driver is
  involved, so the picture is frozen as handed over.
- **Input** is a USB CDC-ACM gadget that the initramfs brings up, showing up as
  `/dev/ttyACM0` on the host. This is why the tree needs the USB complex, the
  PHY, dwc2 and therefore the PMGR power domains.

UART0 exists and `earlycon` uses it, but reading it needs a serial cable on the
Lightning pins.

## Where the numbers come from

The addresses in `dts/t8020.dtsi` are read out of the DeviceTree inside the
iPhone11,6 IPSW, not guessed from T8015. Extract and dump it with:

    ipsw extract --pattern 'DeviceTree.*' -o work <ipsw>
    ipsw dtree --json DeviceTree.d331pap.im4p > refs/iosdt/d331pap.json
    python3 tools/iosdt.py refs/iosdt/d331pap.json uart

Every reg address in that tree is an offset from its parent's base, and that
nests: `/arm-io` sets the window base from its `ranges` property, so
`usb-complex` at `0x39000000` is really `0x239000000`, and `usb-device` at
`0x100000` under it is `0x239100000` rather than `0x200100000`. `tools/iosdt.py`
accumulates the base down the tree. Getting this wrong is silent, so there is a
check for it:

    python3 tools/iosdt.py --selfcheck

Useful findings so far:

| Block         | Address      | Interrupt | Notes                                 |
|---------------|--------------|-----------|---------------------------------------|
| arm-io        | 0x200000000  |           | `arm-io,t8020`, soc-generation H11    |
| uart0         | 0x235200000  | 386       | iBoot `debug-console`, clock gate 76  |
| usb-complex   | 0x239000000  |           | PHY at +0x60, same shape as T8015     |
| usb-device    | 0x239100000  | 451       | dwc2, peripheral mode                 |
| dart-usb      | 0x239900000  | 454       | present in ADT, ignored by this tree  |
| pmgr          | 0x23b000000  |           | `pmgr1,t8020`                         |
| aic           | 0x23b100000  |           | `aic,1`, same generation as T8015 and M1 |
| gpio          | 0x23c100000  | 95 to 101 |                                       |
| pmgr_mini     | 0x23d200000  |           |                                       |
| wdt           | 0x23d2b0000  | 232       |                                       |
| cluster 0 (E) | 0x210000000  |           | 4x Tempest, 2 MB L2                   |
| cluster 1 (P) | 0x211000000  |           | 2x Vortex, 8 MB L2                    |

CPU affinity differs from T8015: iOS reports `reg` 0 for cpu0 and 256 for cpu4,
so the second cluster sits at affinity level 1, giving MPIDRs 0x0 to 0x3 and
0x100 to 0x101.

The iPhone XS (D321AP) DeviceTree was diffed against this one and the SoC blocks
are identical, so adding that board later is just a second `.dts`.

`dts/t8020-pmgr.dtsi` is generated, not written. It comes from m1n1's
`pmgr_adt2dt.py` reading the same ADT, reshaped to the convention the other
Apple trees use. Regenerate with `make pmgr`.

## Build

    make deps       # clone m1n1, the Hoolock kernel, the docs, make a venv
    make check      # compile the device tree, no kernel tree needed
    make m1n1
    make kernel     # Hoolock's config_16k, LLVM=1 ARCH=arm64
    make initramfs
    make blob       # build/m1n1-linux.bin

Then, with the RP2350 plugged into the host and the phone connected through it:

    make pongo      # DFU to a PongoOS shell
    make boot       # upload the blob and chainload it

### RP2350 firmware

l41ka ships no binary in its git tree, only in its releases, and the release is
marked prerelease so the `releases/latest` API returns 404. There is one asset,
for the only board the author tested:

    https://github.com/0cyn/l41ka/releases/download/0.1.0/l41ka-waveshare-rp2350-usb-a.zip

It unpacks to `l41ka.uf2`, `l41ka.elf`, `l41ka.bin`, `l41ka.hex` plus a
disassembly and a link map. `make deps` does not fetch it, so it lives in
`build/l41ka-fw/` by hand. To flash:

    picotool load -t elf build/l41ka-fw/l41ka.elf -x -f

Or hold BOOTSEL and copy `l41ka.uf2` onto the mass storage device that appears,
which needs no picotool at all. Coming from an older l41ka firmware, put the
board in BOOTSEL first with `laikadbg.py bootsel`, or `picotool reboot -f -u`.

The 0.1.0 firmware and the scripts in a current clone are the same version in
practice: the only commit after the release tag is a README screenshot.

Host toolchain notes, since none of this is the default on a Linux box:

- The kernel and m1n1 both need `ld.lld`. There is no need for an aarch64 GCC,
  clang cross compiles on its own.
- m1n1 needs the Rust `core` and `alloc` crates for
  `aarch64-unknown-none-softfloat`. Without rustup, `BUILDSTD=1` builds them
  from source, which needs the Rust standard library sources installed. On
  Gentoo that is the `rust-src` USE flag on `dev-lang/rust-bin`.
- `laikadbg.py` needs `pyusb` and `pyserial` in the venv.

To check whether the phone is pwned, read its DFU serial string:

    cat /sys/bus/usb/devices/*/serial

A pwned iPhone XS Max looks like this, and the fields are worth reading:

    CPID:8020 CPRV:11 CPFM:03 SCEP:01 BDID:1A ECID:... IBFL:3C
    SRTG:[iBoot-3865.0.0.4.7] PWND:[usbliter8]

`CPID:8020` is the chip id, which is where the T8020 name comes from, and
`BDID:1A` is the D331pAP board id.

## Expected driver work

Probably none. Every compatible string falls back to a driver that already
exists in the Hoolock tree:

- `apple,t8020-aic` falls back to `apple,aic`
- `apple,s5l-uart` is the existing Samsung driver
- `apple,t8020-pmgr` and `apple,t8020-pmgr-pwrstate` fall back to the generic
  `apple,pmgr` and `apple,pmgr-pwrstate`
- `apple,t8020-dwc2` falls back to `apple,dwc2`
- `apple,t8020-ausb-phy` falls back to `apple,ausb-phy`

m1n1 also needs no changes for a single core boot: an unknown MIDR falls back to
a null chicken init, `tunables_apply_static` defaults to doing nothing, and an
unrecognised chip in `smp.c` just skips starting the secondaries.

## Status: Linux boots

Mainline Linux 7.3.0-rc1 runs on the iPhone XS Max, on one Vortex core, with a
framebuffer console. The kernel confirms, in its own words, every address taken
out of the IPSW:

    apple-pmgr-pwrstate 23d200000.power-management:power-controller@0058
    235200000.serial: ttySAC0 MMIO32:0x0000000235200000 (irq = 35) is a APPLE S5L
    [drm] Initialized simpledrm 1.0.0 for 8f7cd4000.framebuffer on minor 0
    Console: switching to colour frame buffer device 155x168
    Unpacking initramfs...

So `pmgr_mini`, UART0, the generated power domains and the handover framebuffer
are all correct. What is not yet working is the USB gadget, so there is no
interactive shell: output only.

### What it took, in m1n1

Six patches, in `patches/`. Three were genuine T8020 bugs, three were about
being able to see anything at all.

1. **`0001` USB complex registers.** `usb-complex,t8020` fell through to the
   `t8011` case and got its DMA remap offsets. The real ones are in the ADT's
   `usb_widget_with_mask`: 0x28, 0x38, 0x48.
2. **`0005` A12 MIDR parts.** `midr.h` already defined
   `MIDR_PART_T8020_VORTEX` and `_TEMPSET` but nothing referenced them, so the
   lookup fell back to `features_unknown` with `apple_sysregs_unlocked` false,
   which sent an A12 down an M4-only path reading `CTRR_M4_LWR_EL2` from EL1.
3. **`0002` SError handling.** An asynchronous SError fires at the top-of-RAM
   boundary, `L2C_ERR_ADR` 0x8ec78c000, every boot. m1n1's existing clear is
   `reg_mask(..., ACCESS_FAULT | PIO_FAULT, 0)`, a write of zeroes, but the
   register is write-1-to-clear: `ENABLE_W1C`, bit 56, is set. So that clear
   has never worked here. Writing ones to the fault bits, `RECURSIVE_FAULT`
   included, does. The underlying cause of the error is still unknown.

The other three, `0003`, `0004` and `0006`, exist because the framebuffer is
the only console without a serial cable, and a `RELEASE` build defines
`FB_SILENT_MODE` which silences it exactly when a payload is present.

### Device tree gotcha worth repeating

CPU `reg` must equal `MPIDR_EL1 & 0xFFFFFF`. The ADT's `reg` is **not** that
number: it reports 0 for cpu0 and 256 for cpu4, omitting the Aff2 bit. The boot
core reports MPIDR 0x80010100, so cluster 1 core 0 is 0x10100. With the ADT
values taken at face value, m1n1 prunes all six CPU nodes including the one it
is running on, and Linux gets a device tree with no CPUs.

## Where this stopped: the shell is on screen, not on the host

Linux reaches `HoolockLinux test ramdisk shell` reliably, with the gadget bound
on the phone side:

    dwc2 239100000.usbdev: EPs: 9, dedicated fifos, 2056 entries in SPRAM
    dwc2 239100000.usbdev: bound driver configfs-gadget.g1
    [HL-rd]: Starting uwudhcpd with server ip 172.16.42.1, client ip: 172.16.42.2

The phone is 172.16.42.1 and runs a DHCP server; the host is meant to get
172.16.42.2 and `telnet 172.16.42.1`. That last hop does not work yet.

### Why this is harder here than on A11

The A11 port never hits it. checkm8 runs from the PC, so the phone is attached
to the PC for the whole boot and the gadget enumerates there naturally.
usbliter8 needs the RP2350 as the USB host, so our phone is on the jig exactly
when dwc2 probes at ~0.26s, and it binds its pullup to the jig. Moving the
cable afterwards does nothing: the device never re-presents itself, so the new
host never enumerates it.

### What was tried

- **Hot-swapping the cable after boot.** Never enumerated, in any port.
- **A powered hub and a USB-C adapter**, following the wiki's note that AMD
  hosts "may require using a USB adapter". Made things worse, nothing
  enumerated at all, and a `VIA Labs USB Billboard Device` appeared in the
  chain. The host here is a Ryzen 5 2600.
- **A 30 second pause in m1n1 before vectoring**, to move the cable before
  dwc2 probes. This is in `patches/m1n1-0003`. Inconclusive so far.
- **Rebinding the UDC from a script injected into the initramfs**, writing ""
  then the controller name to `/sys/kernel/config/usb_gadget/g1/UDC` to toggle
  the pullup. The gadget *did* enumerate once this way, as
  `05ac:12a8` with a working CDC interface. But any repacked initramfs panics
  the kernel on unpack, and this was reverted. Do not re-add it without
  solving that first.

### The repack problem, unexplained

Appending a second cpio member to Hoolock's initramfs and re-gzipping makes the
kernel die immediately at unpack. Ruled out: the base archive is 3379712 bytes,
`mod 512 == 0`, so alignment is fine; the appended member is present with mode
0755 and a valid `#!` line; `sh -n` passes. The stock initramfs boots every
time, the repacked one never does. Whatever is wrong is in how the archive is
assembled, not in the script.

### Next things to try

1. Rebuild the initramfs as a **single** cpio rather than appending a second
   member, which sidesteps multi-archive semantics entirely.
2. Configure the host interface by hand once it does enumerate. This needs
   root, which the agent does not have:
   `sudo ip link set <iface> up && sudo ip addr add 172.16.42.2/24 dev <iface>`
3. Failing all that, a serial cable on the Lightning pins makes UART0 readable
   and sidesteps USB for debugging, though not for a usable shell.

### An unrelated flake worth knowing

The chain intermittently fails with the board dropping off USB mid-`exploit`.
`chain.py` reopens, finds the phone already in `recovery`, and says so rather
than continuing. The phone then boots iOS. Just retry; it usually takes on the
next attempt.

## Known gaps and the order to attack them

1. **SMP.** m1n1 has no `cpu_start_off` for T8020, hence `maxcpus=1`. Fixing
   this means finding the CPU start offset, somewhere between the S8000 and
   T8103 values.
2. **DMA.** The USB bus uses a `dma-ranges` identity map like T8015, which
   assumes the USB DART is in bypass. If USB DMA misbehaves, this is the first
   thing to check.
3. **cpufreq.** Needs measured A12 operating points. Cluster controllers are at
   0x210e20000 and 0x211e20000.
4. **Chicken bits.** A12 Vortex and Tempest are MIDR parts 0xb and 0xc. Running
   without them is fine for bring-up but not for stability.
5. **Notch.** m1n1 only trims the notch for T8015 board ids 0x6 and 0xe, and
   this phone has no `partially-occluded-display` in its ADT `/product`, so the
   framebuffer will include the notch area. Cosmetic.

## m1n1 needs a T8020 USB complex case

m1n1 picks its USB complex register layout by compatible string, trying
`s5l8960x`, then `t8015`, then falling through to `t8011`. Our ADT says:

    /arm-io/usb-complex  compatible: ['usb-complex,t8020', 'usb-complex,t8011']

so a T8020 silently got T8011's DMA remap offsets, `0x18/0x24/0x74/0x84`. That
fallback is not good enough: T8015 carries the same `t8011` fallback and still
needs its own offsets, and T8020 has a different controller set again, with
`usb-device`, `usb-ehci0` and `usb-ohci0` but no second EHCI.

The real offsets are in the ADT, in `/arm-io/usb-complex usb_widget_with_mask`,
as three (offset, value, mask) triplets:

    offset 0x028  value 0x00000088  mask 0x030000ff
    offset 0x038  value 0x00000088  mask 0x030000ff
    offset 0x048  value 0x00000088  mask 0x030000ff

The mask covers bits 0-7 and 24-25, which is exactly m1n1's T8011 bit layout
(`USBX_REMAP_TO_DRAM_BITS_T8011` is `0x3000088`), so only the offsets differ.
`patches/m1n1-0001-t8020-usb-complex.patch` adds a `USBCOMPLEX_T8020` case
writing the T8011 values to 0x28, 0x38 and 0x48.

Why this is worth suspecting as a crash cause: writing remap-enable bits to the
wrong registers leaves USB DMA un-remapped, so transfers land at wrong physical
addresses. That corrupts memory rather than faulting cleanly, which matches a
crash that happens a while after the logo and some console output.

It also explains why m1n1 with no payload rebooted instead of hanging.
`run_actions()` ends in `uartproxy_run()`, which blocks forever, but only if
USB came up. If it returns, `main()` falls through to `panic("Nothing to do!")`,
and a panic on an iDevice reboots. So m1n1 alone staying on screen is the
signal that USB init is fixed.
