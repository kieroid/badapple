# linux4a12

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP).

The goal is a console and an initramfs shell, the same finish line as the A10
and A11 ports. No display driver, touch, wifi, cellular or GPU.

The phone's NAND now holds an ext4 filesystem, not iOS. Everything here is
tethered: nothing on the flash is read to boot, so wiping it does not matter.

## Status

| | |
|---|---|
| Boot to a root shell | works |
| Framebuffer console | works (handover from iBoot) |
| USB gadget, telnet over CDC-NCM | works |
| AIC interrupts | works |
| NVMe storage | works, reads and writes, ext4 mounted and verified |
| SMP | works, all six cores |
| cpufreq | OPP tables written from the ADT, never booted |
| Boot chain without iBoot | in progress, PongoOS does not come up yet |
| display, everything else | not started |

    $ telnet 172.16.42.1
    ~ # uname -a
    Linux (none) 7.3.0-rc1 #33 SMP PREEMPT aarch64 Linux
    ~ # cat /proc/partitions
     259        0   62500000 nvme0n1
    ~ # mount /dev/nvme0n1 /mnt && df -h /mnt | tail -1
    /dev/nvme0n1    58.4G    321.4M   55.1G   1% /mnt

Details, register dumps and the debugging history are in
[NOTES.md](NOTES.md).

## Boot chain

    SecureROM
      -> usbliter8, run from a Pico 2 W jig, leaves the phone in pwned DFU
      -> PongoOS, uploaded to the bootrom over DFU by tools/dfuboot.py
      -> m1n1, uploaded to PongoOS by tools/pcboot.py
      -> Linux, 16K pages, t8020-d331p.dtb, initramfs

iBoot is not in that list, and that part does not work yet. See
[what is broken](#what-is-broken).

An earlier version went through iBoot, with the jig firmware patching its
auto-boot path to install a second stage. That stopped working as soon as Linux
took the NAND and there was no OS left to auto-boot, and that firmware carries
no license, so it is gone. Handing PongoOS straight to the pwned bootrom reads
nothing off the flash, so once it works, wiping the phone cannot break the boot
chain again.

## What is broken

`make pongo` uploads PongoOS to the pwned bootrom and the phone goes dark: no
USB, no console, and eventually a watchdog reset back to DFU.

What is known:

- The exploit is fine. The phone reports `PWND:[usbliter8]` and takes the
  upload.
- PongoOS since 2.6.0 relocates itself into SRAM and wants the SRAM base
  handed to it in SP, falling back to `0x180000000`, which its own comment
  says is right for pre-A12 only. The bootrom passes nothing.
  `tools/a12-pongo-shim.S` sets SP to T8020's `0x19c000000`, taken from where
  the exploit's own offsets point. That did not fix it on its own.
- Still unchecked: whether PongoOS is entered at the exception level it
  expects, and whether `x30` may be null.

One consequence either way: iBoot is what sets the display up, and `console=tty0`
is a handover of the framebuffer it leaves behind. With iBoot out of the boot
chain there is no framebuffer, so the on-screen console goes away and telnet
becomes the only one.

## Build

    make deps       # clone m1n1, the Hoolock kernel and docs, set up a venv, apply patches/
    make pongoos    # build PongoOS with T8020 support
    make check      # compile the device tree on its own
    make blob       # m1n1, kernel, initramfs, all in build/m1n1-linux.bin

`make patch` re-applies `patches/` and is safe to run again. A patch that is
already applied is skipped, and a real conflict stops the build.

## Run

    # phone in DFU on the jig, the exploit fires by itself in about a second
    # move the Lightning cable from the jig to the PC
    make pongo      # upload PongoOS to the pwned bootrom and run it
    make pc-boot    # upload the blob over PongoOS's USB and boot

`make pongo` does not currently reach a PongoOS shell, see
[what is broken](#what-is-broken).

About twenty seconds later the phone shows up as `05ac:4142`, hands the PC
`172.16.42.2` over DHCP, and answers telnet on `172.16.42.1`.
`tools/tsh.py "cmd"` runs one command there from a script.

If `make pongo` cannot find a pwned device, check the phone's USB serial for
`PWND:[usbliter8]`. Without it the exploit has not run: unplug the jig from USB
for a few seconds, a firmware reset is not enough, and put the phone back in
DFU.

## Dependencies

| Host | For |
|---|---|
| `clang`, `ld.lld` | kernel and m1n1, no aarch64 GCC needed |
| Rust with `rust-src` | m1n1 (`BUILDSTD=1`). On Gentoo a USE flag on `rust-bin` |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/`, m1n1's proxyclient |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw`, `pyimg4` | only to pull a DeviceTree out of a new IPSW |

`build/venv` gets `pyusb`, `pyserial` and `construct`. The jig firmware is
built separately, see [NOTES.md](NOTES.md#rp2350-firmware).

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/` keeps the license of
the code it patches: `linux-*` GPL-2.0, `m1n1-*` MIT. Nothing third party is
vendored here; what `make deps` fetches is listed in [NOTICES.md](NOTICES.md).
