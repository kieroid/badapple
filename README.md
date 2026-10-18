# Bad Apple

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP). The goal is a console and a shell.

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
| Rust with `rust-src` | m1n1 |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/`, m1n1's proxyclient |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw` | the IPSW and img4 |
| `irecovery` | recovery mode |
| [usbliter8ra1n-patch](https://github.com/Trussh/usbliter8ra1n-patch) | jig firmware, see [NOTES.md](NOTES.md#the-jig) |

And an iPhone11,6 IPSW.

## Build

```sh
make deps                                  # clone m1n1, Linux, Hoolock's docs; apply patches/
make images IPSW=/path/to/unpacked/ipsw    # once: iBSS, device tree, trustcache, touch firmware
make boot                                  # builds the rest and boots
```

`make check` builds only the device tree and works on a bare checkout.

## Run

Put the phone in DFU on the jig. The exploit fires in about a second. Move the
cable to the PC and run `make boot`.

The phone shows up as `05ac:4142` with telnet on `172.16.42.1`.
`tools/tsh.py "cmd"` runs one command. The first USB attach often fails and the
phone retries, so give it a minute.

No `PWND:[usbliter8]` in the USB serial means the exploit did not fire. Unplug
the jig for a few seconds and try again.

## Status

- [x] Boot chain, tethered
- [x] Device tree, PMGR domains generated from the ADT
- [x] Display
  - [x] Console, notch cut out
  - [x] Display driver, XRGB8888 and XRGB2101010
  - [x] Brightness, `/sys/class/backlight/230400000.dsi.0`
  - [ ] HDR: high brightness mode, gamma tables
- [x] AIC interrupts and fast IPIs
- [x] SMP on all six cores
  - [ ] Chicken bits per core in the kernel, m1n1 sets them once
- [x] NVMe, ext4
- [x] SMC: battery, charger, buttons, ringer switch, poweroff, reboot
- [x] Die temperature, hwmon, thermal zones at 85 C
- [x] USB gadget: telnet, DHCP, getty on `/dev/ttyGS0`
  - [ ] Why the first attach fails
- [x] Touch, `/dev/input/event1`, with its interrupt
  - [ ] Power off, force sensor
- [x] cpufreq: E up to 1572 MHz, P up to 2376 MHz
  - [ ] Boost, 2496 MHz
- [ ] Wifi, cellular, GPU, audio
- [ ] Upstream

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/linux-*` are GPL-2.0,
`patches/m1n1-*` MIT. Dependencies are in [NOTICES.md](NOTICES.md).
