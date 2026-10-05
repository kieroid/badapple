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
