# Bad Apple

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP).

The target is a console and a shell, the same finish line as the A10 and A11
ports. No touch, wifi, cellular or GPU.

The phone's NAND holds ext4, not iOS. The boot is tethered and reads nothing
off the flash, so wiping it does not break anything.

Registers, dead ends and the debugging history are in [NOTES.md](NOTES.md).

## Boot chain

```text
SecureROM
  -> usbliter8 on a Pico 2 W jig, fires the bootrom exploit, leaves pwned DFU
  -> iBSS, patched to stop checking signatures, uploaded by tools/dfuboot.py
  -> device tree and an empty trustcache, over Apple's recovery protocol
  -> m1n1, handed to bootx where a restore kernelcache would go
  -> Linux, 16K pages, t8020-d331p.dtb, initramfs
```

iBSS stays in the chain on purpose. It brings up DRAM, the ADT and the display,
and `console=tty0` is the framebuffer it leaves behind.

## Dependencies

| Host | For |
|---|---|
| `clang`, `ld.lld` | kernel and m1n1, no aarch64 GCC needed |
| Rust with `rust-src` | m1n1 (`BUILDSTD=1`). On Gentoo a USE flag on `rust-bin` |
| `dtc`, `cpp` | `make check` |
| `python3` | `tools/`, m1n1's proxyclient |
| `git`, `curl`, `unzip` | `make deps`, `make initramfs` |
| `ipsw`, `pyimg4` | unpacking the IPSW, wrapping images in img4 |
| `irecovery` | talking to iBSS in recovery mode |
| [usbliter8ra1n-patch](https://github.com/Trussh/usbliter8ra1n-patch) | the jig firmware, built and flashed once |

`make deps` clones m1n1, the Hoolock kernel and docs into `build/`, makes
`build/venv` with `pyusb`, `pyserial` and `construct`, and applies `patches/`.
`tools/dfuboot.py` needs that venv, since `pyusb` is what talks to DFU.
The jig firmware is built separately, see
[NOTES.md](NOTES.md#rp2350-firmware).

You also need an iPhone11,6 IPSW. Everything below takes files out of it.

## Build

```sh
make deps       # clone, venv, apply patches/
make check      # compile the device tree on its own
make m1n1       # build the m1n1 payload
make initramfs  # download Hoolock's test initramfs
make kernel     # build Image.gz with Hoolock's 16K config
```

`make boot` rebuilds m1n1, the device tree and the kernel as needed, so those
are only separate targets when you want one of them on its own.

`make check` is the only target that works on a bare checkout. `make patch`
re-applies `patches/` and is safe to run again: an applied patch is skipped, a
real conflict stops the build.

cpufreq is disabled by the build, not by you. Frequency requests are accepted
and then never completed, because the coprocessor that arbitrates DVFS is not
running, see
[NOTES.md](NOTES.md#cpufreq-the-part-that-completes-transitions-is-not-running).

## First time setup

Done once, from an unpacked IPSW. It patches iBSS so it will load a payload
Apple never signed, and wraps the device tree and an empty trustcache for
`bootx`.

```sh
make images IPSW=/path/to/unpacked/ipsw
```

Everything lands in `build/iboot/`. Any manifest does once iBSS is patched, so
the IM4M is Hoolock's `0x8015.im4m`. There is no `0x8020` template and there
cannot be: from A11 on a manifest is tied to the device's own ECID.

## Run

Put the phone in DFU on the jig. The exploit fires by itself in about a second.
Move the cable to the PC, then:

```sh
make boot
```

That packs m1n1, the device tree, the kernel and the initramfs into one fake
restore kernelcache and hands the whole chain to the phone. `make kernelcache`
builds it without booting anything.

Within about half a minute the phone is `05ac:4142`, hands the PC
`172.16.42.2` over DHCP, and answers telnet on `172.16.42.1`.
`tools/tsh.py "cmd"` runs one command there from a script.

The first USB attach usually fails and the gadget retries by itself, so the
link can take a few tries to appear. Nothing to do but wait, see
[NOTES.md](NOTES.md#the-self-replug).

If it says the phone is not pwned, check its USB serial for `PWND:[usbliter8]`.
Without it the exploit has not run: unplug the jig from USB for a few seconds,
a firmware reset is not enough, and put the phone back in DFU.

## Implementation status

- [x] Boot chain, tethered, nothing read off the NAND
  - [x] Patched iBSS that takes an unsigned payload
  - [x] m1n1 as a fake restore kernelcache, with a real BootArgs
- [x] Device tree for T8020, PMGR domains generated from the ADT
- [x] Console
  - [x] Framebuffer, handed over by iBoot
  - [x] Display driver, see
        [NOTES.md](NOTES.md#the-driver-works-and-why-the-panel-was-black)
  - [x] Display colours on the console
  - [x] Brightness, through `/sys/class/backlight/230400000.dsi.0`
  - [x] Notch trimmed out of the framebuffer, 2688 down to 2556
- [x] AIC interrupts, including fast IPIs
- [x] SMP on all six cores
  - [ ] Chicken bits reapplied per core by the kernel, m1n1 does it once
- [x] NVMe through ANS2, reads and writes, ext4 mounted
- [x] SMC
  - [x] Battery: capacity, health, cycles, voltage, current, temperature
  - [x] Charger detection and the power button
  - [x] Poweroff, with the PMU scratchpad over SPMI so it stays off
  - [x] Reboot, through the watchdog, lands back in recovery
- [x] Die temperature sensors, eight channels through hwmon
- [x] USB
  - [x] Gadget: telnet, DHCP, getty on `/dev/ttyGS0`
  - [x] DART, in m1n1 and through `apple-dart` in Linux
  - [x] Comes up on every boot, the gadget replugs itself until the host takes it
  - [ ] Why the first attach fails at all. Worked around, not understood
- [ ] cpufreq. Needs the PMP/CLPC coprocessor that completes transitions
- [ ] Touch, wifi, cellular, GPU, audio
- [ ] Anything upstream

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/` keeps the license of the
code it patches: `linux-*` GPL-2.0, `m1n1-*` MIT. Nothing third party is
vendored here. What the boot depends on is listed in [NOTICES.md](NOTICES.md).
