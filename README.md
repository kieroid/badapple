# linux4a12

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP).

The goal is a console and an initramfs shell, the same finish line as the A10
and A11 ports. No display driver, touch, wifi, cellular or GPU.

Nothing is written to the phone. A reboot returns it to iOS.

## Status

| | |
|---|---|
| Boot to a root shell | works |
| Framebuffer console | works (handover from iBoot) |
| USB gadget, telnet over CDC-NCM | works, fails to enumerate on roughly 1 boot in 4 |
| AIC interrupts | works |
| NVMe storage | reads work, about 400 MB/s, writes untested |
| SMP | m1n1 starts all six cores, Linux hangs once it releases them, so `maxcpus=1` |
| cpufreq, display, everything else | not started |

    $ telnet 172.16.42.1
    ~ # uname -a
    Linux (none) 7.3.0-rc1 #33 SMP PREEMPT aarch64 Linux
    ~ # cat /proc/partitions
     259        0   62500000 nvme0n1
     259        1   62499956 nvme0n1p1

Details, register dumps and the debugging history are in
[NOTES.md](NOTES.md).

## Boot chain

    SecureROM
      -> usbliter8, run from an RP2350 jig (l41ka)
      -> patched iBSS / iBEC
      -> PongoOS, bootm
      -> m1n1
      -> Linux, 16K pages, t8020-d331p.dtb, initramfs

l41ka is the only thing that gets an A12 this far. Its license is "All Rights
Reserved", so it stays in `build/` and none of it is vendored here.

## Build

    make deps       # clone m1n1, the Hoolock kernel and docs, set up a venv, apply patches/
    make check      # compile the device tree on its own
    make blob       # m1n1, kernel, initramfs, all in build/m1n1-linux.bin

`make patch` re-applies `patches/` and is safe to run again. A patch that is
already applied is skipped, and a real conflict stops the build.

## Run

    make pongo      # phone in DFU on the jig, ends at a PongoOS shell
                    # move the Lightning cable from the jig to the PC
    make pc-boot    # upload over PongoOS's USB and boot

About twenty seconds later the phone shows up as `05ac:4142`, hands the PC
`172.16.42.2` over DHCP, and answers telnet on `172.16.42.1`.
`tools/tsh.py "cmd"` runs one command there from a script.

If the exploit stage keeps failing, unplug the RP2350 from USB for a few
seconds. A firmware reset is not enough.

## Dependencies

| Host | For |
|---|---|
| `clang`, `ld.lld` | kernel and m1n1, no aarch64 GCC needed |
| Rust with `rust-src` | m1n1 (`BUILDSTD=1`). On Gentoo a USE flag on `rust-bin` |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/`, m1n1's proxyclient |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw`, `pyimg4` | only to pull a DeviceTree out of a new IPSW |

`build/venv` gets `pyusb`, `pyserial` and `construct`. The l41ka firmware and
scripts go in `build/l41ka/` by hand, see
[NOTES.md](NOTES.md#rp2350-firmware).

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/` keeps the license of
the code it patches: `linux-*` GPL-2.0, `m1n1-*` MIT.
