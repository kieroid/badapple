# Bad Apple

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP). The goal is a console and a shell. No wifi, cellular or GPU.

The boot is tethered and reads nothing off the NAND, so the NAND can hold ext4.

Registers and dead ends are in [NOTES.md](NOTES.md).

## Boot chain

```text
SecureROM
  -> usbliter8 on a Pico 2 W jig, leaves the phone in pwned DFU
  -> iBSS, patched to skip signature checks, sent by tools/dfuboot.py
  -> device tree and an empty trustcache, over Apple's recovery protocol
  -> m1n1, in place of a restore kernelcache
  -> Linux, 16K pages, initramfs
```

iBSS stays in the chain. It brings up DRAM and the display.

## Dependencies

| Host | For |
|---|---|
| `clang`, `ld.lld` | kernel and m1n1 |
| Rust with `rust-src` | m1n1 (`BUILDSTD=1`) |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/`, m1n1's proxyclient |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw`, `pyimg4` | unpacking the IPSW, img4 |
| `irecovery` | recovery mode |
| [usbliter8ra1n-patch](https://github.com/Trussh/usbliter8ra1n-patch) | jig firmware, flashed once, see [NOTES.md](NOTES.md#the-jig) |

You also need an iPhone11,6 IPSW.

## Build

```sh
make deps       # clone m1n1, Linux and Hoolock's docs, make build/venv, apply patches/
make check      # compile the device tree
make m1n1
make initramfs  # download Hoolock's test initramfs
make kernel
```

`make boot` rebuilds what it needs, so the other targets are optional.
`make check` is the only one that works on a bare checkout. `make patch` is
safe to run again.

## First time setup

```sh
make images IPSW=/path/to/unpacked/ipsw
```

This patches iBSS, wraps the device tree and trustcache, and converts the touch
firmware. All of it lands in `build/iboot/` and stays there.

## Run

Put the phone in DFU on the jig. The exploit fires in about a second. Move the
cable to the PC, then:

```sh
make boot
```

Within half a minute the phone shows up as `05ac:4142` and answers telnet on
`172.16.42.1`. `tools/tsh.py "cmd"` runs one command. The first USB attach
usually fails and the phone retries by itself, so give it time.

If the phone is not pwned, its USB serial has no `PWND:[usbliter8]`. Unplug the
jig for a few seconds and put the phone back in DFU.

## Status

- [x] Boot chain, tethered
- [x] Device tree, PMGR domains generated from the ADT
- [x] Display
  - [x] Console on the framebuffer, notch cut out
  - [x] Display driver
  - [x] Brightness, `/sys/class/backlight/230400000.dsi.0`
- [x] AIC interrupts and fast IPIs
- [x] SMP on all six cores
  - [ ] Chicken bits per core in the kernel, m1n1 sets them once
- [x] NVMe, ext4
- [x] SMC
  - [x] Battery and charger
  - [x] Side button, volume buttons, ringer switch
  - [x] Poweroff and reboot
- [x] Die temperature, hwmon
- [x] USB gadget: telnet, DHCP, getty on `/dev/ttyGS0`
  - [ ] Why the first attach fails
- [x] Touch, `/dev/input/event1`
  - [ ] Real interrupt, power off, force sensor
- [ ] cpufreq, needs the CLPC coprocessor
- [ ] Wifi, cellular, GPU, audio
- [ ] Upstream

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/linux-*` are GPL-2.0,
`patches/m1n1-*` MIT. Dependencies are in [NOTICES.md](NOTICES.md).
