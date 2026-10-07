# linux4a12

Mainline Linux on the Apple A12 (T8020), targeting the iPhone XS Max
(iPhone11,6, board D331pAP).

Scope: serial console and an initramfs shell. No display driver, touch, wifi,
cellular or GPU. Same finish line as the A10 and A11 ports.

Nothing is written to the phone. Reboot returns it to iOS.

Detailed findings, register dumps and the debugging history are in
[NOTES.md](NOTES.md).

## Status

It boots to a root shell.

Linux 7.3.0-rc1 runs on one Vortex core with a framebuffer console. UART0, the
generated power domains, the handover framebuffer, AIC interrupts and the USB
gadget all work. The gadget enumerates as `05ac:4142` and brings up a CDC-NCM
network interface alongside a CDC-ACM serial port, so the phone answers on
`172.16.42.1`:

    telnet 172.16.42.1

    HoolockLinux test ramdisk shell

    ~ # uname -a; id
    Linux (none) 7.3.0-rc1 #33 SMP PREEMPT aarch64 Linux
    uid=0 gid=0

Interrupts arrive on both devices that have them:

      34:      10926  AIC-FIQ     1 Level     arch_timer
      35:          5  AIC      65922 Level     235200000.serial
      36:        284  AIC      65987 Level     239100000.usbdev

m1n1's own gadget also works, which is what `proxy-boot` uses.

That is the scope this port set out to reach. What is left is hardening: SMP,
the gaps listed at the end, and making the patches fit to send upstream.

## Boot chain

    SecureROM
      -> usbliter8 (RP2350 sends crafted USB setup packets in DFU)
      -> PWND DFU
      -> patched iBSS / iBEC
      -> PongoOS
      -> bootm
      -> m1n1
      -> Linux (16K pages) + t8020-d331p.dtb + initramfs

l41ka is the only thing that gets an A12 this far. It runs usbliter8 from the
RP2350 and embeds a PongoOS fork with T8020 support and the `bootm` command,
neither of which upstream PongoOS has. It is "All Rights Reserved", so it stays
a tool in `build/` and none of it is vendored here.

## Build

    make deps       # clone m1n1, the Hoolock kernel, the docs, make a venv
    make check      # compile the device tree, no kernel tree needed
    make m1n1
    make kernel     # Hoolock's config_16k, LLVM=1 ARCH=arm64
    make initramfs
    make blob       # build/m1n1-linux.bin

## Running

Preferred. The phone leaves the jig once, before anything boots, so Linux comes
up with the cable on the host for the whole boot:

    make pongo      # jig: DFU to a PongoOS shell
                    # move the Lightning cable from the jig to this PC
    make pc-boot    # upload and chainload over PongoOS's gadget (05ac:4141)

About twenty seconds later the phone is up as `05ac:4142`. The host takes
`172.16.42.2` by DHCP from it, and the shell is on the other end:

    telnet 172.16.42.1

There is a getty on the CDC-ACM port too, as the `05ac:4142` `/dev/ttyACM*`
rather than the jig's. It wants a keypress before it prints anything.

m1n1 alone, which is how its proxy becomes usable:

    make pongo
                    # move the cable to the PC
    build/venv/bin/python tools/pcboot.py build/m1n1/build/m1n1.bin

It lands in `uartproxy_run()` and enumerates as `1209:316d`. Then:

    make proxy-boot # load kernel, dtb and initramfs, no new DFU cycle

One proxy session per m1n1 boot. A second one wedges it.

### When the chain starts failing

Retry first. The board drops off USB mid-`exploit` and the phone boots iOS.

It degrades the more the phone comes off the jig. A firmware reset is not
enough: unplug the RP2350 from USB for a few seconds and plug it back in.

## Dependencies

Host tools:

| Need                      | Why                                          |
|---------------------------|----------------------------------------------|
| `clang`, `ld.lld`         | kernel and m1n1; no aarch64 GCC needed       |
| Rust with `rust-src`      | m1n1's `core`/`alloc` via `BUILDSTD=1`       |
| `dtc`, `cpp`              | `make check`                                 |
| `python3` with `venv`     | `tools/`, m1n1's proxyclient                 |
| `git`, `curl`, `unzip`    | `make deps`, `make initramfs`                |
| `cpio`, `gzip`            | initramfs repack                             |
| `ipsw`                    | only for `make pmgr` and refreshing the ADT  |

Python, into `build/venv` (`make deps` installs `construct`):

| Package     | Used by                                  |
|-------------|------------------------------------------|
| `pyusb`     | `laikadbg.py`, `tools/pcboot.py`         |
| `pyserial`  | m1n1 proxyclient                         |
| `construct` | m1n1 proxyclient ADT parsing             |

Cloned into `build/` by `make deps`:

| Repo                          | Role                              |
|-------------------------------|-----------------------------------|
| `HoolockLinux/m1n1`           | chainloader                       |
| `HoolockLinux/linux`          | kernel                            |
| `HoolockLinux/docs`           | `config_16k`                      |

Fetched by hand, not by `make deps`:

| Thing                    | Where                                                |
|--------------------------|------------------------------------------------------|
| l41ka firmware + scripts | `build/l41ka/`, see [NOTES.md](NOTES.md#rp2350-firmware) |
| PongoOS source           | `build/pongoos/`, reference only, pin the `t8030` branch |
| Hoolock initramfs        | `make initramfs`, from nightly.link                  |

On Gentoo, `rust-src` is a USE flag on `dev-lang/rust-bin`.

## Addresses

Read out of the DeviceTree in the iPhone11,6 IPSW, not guessed from T8015.

| Block         | Address      | IRQ       | Notes                                 |
|---------------|--------------|-----------|---------------------------------------|
| arm-io        | 0x200000000  |           | `arm-io,t8020`, soc-generation H11    |
| uart0         | 0x235200000  | 386       | iBoot `debug-console`, clock gate 76  |
| usb-complex   | 0x239000000  |           | PHY at +0x60                          |
| usb-device    | 0x239100000  | 451       | dwc2, peripheral mode                 |
| dart-usb      | 0x239900000  | 454       | in the ADT, ignored by this tree      |
| pmgr          | 0x23b000000  |           | `pmgr1,t8020`                         |
| aic           | 0x23b100000  |           | `aic,1`                               |
| gpio          | 0x23c100000  | 95 to 101 |                                       |
| pmgr_mini     | 0x23d200000  |           |                                       |
| wdt           | 0x23d2b0000  | 232       |                                       |
| cluster 0 (E) | 0x210000000  |           | 4x Tempest, 2 MB L2                   |
| cluster 1 (P) | 0x211000000  |           | 2x Vortex, 8 MB L2                    |

`dts/t8020-pmgr.dtsi` is generated by `make pmgr`, not written by hand.

The iPhone XS (D321AP) DeviceTree was diffed against this one and the SoC
blocks are identical, so adding that board later is a second `.dts`.

## License

Dual licensed, `GPL-2.0+ OR MIT`. See [LICENSE](LICENSE).

`patches/` is excluded: those are changes to other people's code and carry that
code's license. `linux-*` are GPL-2.0, `m1n1-*` are MIT.

## Known gaps

1. **Nothing applies `patches/`.** A clean checkout builds m1n1 and the kernel
   unpatched and will not boot.
2. **SMP.** No `cpu_start_off` for T8020 in m1n1, hence `maxcpus=1`.
3. **DMA.** The USB bus uses a `dma-ranges` identity map, assuming the USB DART
   is in bypass. l41ka's author vendored m1n1's DART driver into their PongoOS
   fork, which suggests that assumption is wrong.
4. **cpufreq.** Needs measured A12 operating points. Cluster controllers at
   0x210e20000 and 0x211e20000.
5. **Chicken bits.** A12 Vortex and Tempest are MIDR parts 0xb and 0xc. Fine
   for bring-up, not for stability.
6. **Notch.** m1n1 only trims it for T8015 board ids 0x6 and 0xe, so the
   framebuffer includes the notch area. Cosmetic.
