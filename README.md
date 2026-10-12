# Bad Apple

Mainline Linux on the Apple A12 (T8020), on an iPhone XS Max (iPhone11,6,
D331pAP).

The target is a console and a shell, the same finish line as the A10 and A11
ports. No display driver, touch, wifi, cellular or GPU.

The phone's NAND holds ext4, not iOS. The boot is tethered and reads nothing
off the flash, so wiping it does not break anything.

## Status

| | |
|---|---|
| Boot to a root shell | works |
| Framebuffer console | works, handed over by iBoot |
| USB gadget, telnet over the cable | works |
| AIC interrupts | works |
| NVMe storage | works, reads and writes, ext4 mounted |
| SMP | works, all six cores |
| cpufreq | hangs the boot, build with it off |
| Display, touch, wifi, cellular, GPU | not started |

```console
$ telnet 172.16.42.1
~ # uname -a
Linux (none) 7.3.0-rc1 #33 SMP PREEMPT aarch64 Linux
~ # cat /proc/partitions
 259        0   62500000 nvme0n1
~ # mount /dev/nvme0n1 /mnt && df -h /mnt | tail -1
/dev/nvme0n1    58.4G    321.4M   55.1G   1% /mnt
```

Registers, dead ends and the debugging history are in [NOTES.md](NOTES.md).

## Boot chain

```text
SecureROM
  -> usbliter8 on a Pico 2 W jig, fires the bootrom exploit, leaves pwned DFU
  -> iBSS, patched to stop checking signatures, uploaded by usbliter8ctl
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
| [usbliter8ra1n-patch](https://github.com/Trussh/usbliter8ra1n-patch) | `usbliter8ctl`, which uploads iBSS over pwned DFU |

`make deps` clones m1n1, the Hoolock kernel and docs into `build/`, makes
`build/venv` with `pyusb`, `pyserial` and `construct`, and applies `patches/`.
The jig firmware is built separately, see
[NOTES.md](NOTES.md#rp2350-firmware).

You also need an iPhone11,6 IPSW. Everything below takes files out of it.

## Build

```sh
make deps       # clone, venv, apply patches/
make check      # compile the device tree on its own
make m1n1       # build the m1n1 payload
make initramfs  # download Hoolock's test initramfs
make kernel KCONFIG_EXTRA="--disable CONFIG_ARM_APPLE_SOC_CPUFREQ"
```

`make check` is the only target that works on a bare checkout. `make patch`
re-applies `patches/` and is safe to run again: an applied patch is skipped, a
real conflict stops the build.

cpufreq has to be off. `apple_soc_cpufreq_module_init` never returns and the
boot stops there.

## First time setup

Done once, from an unpacked IPSW. Everything lands in `build/iboot/`.

```sh
mkdir -p build/iboot

# iBSS and the device tree out of the IPSW
ipsw img4 im4p extract -o build/iboot/iBSS.raw \
    <ipsw>/Firmware/dfu/iBSS.d331p.RELEASE.im4p
ipsw img4 im4p extract -o build/iboot/DeviceTree.bin \
    <ipsw>/Firmware/all_flash/DeviceTree.d331pap.im4p

# stop iBSS checking what is inside an img4
python3 tools/iboot-image4-patch.py \
    build/iboot/iBSS.raw build/iboot/iBSS.patched.raw

# a trustcache with no entries, which is all iBoot wants here
printf '\x01\x00\x00\x00\x6e\x17\x68\x17\x88\x2d\x44\x40\x83\xb6\xf3\x22\x06\xbc\x4f\x17\x00\x00\x00\x00' \
    > build/iboot/empty_trustcache.bin

# wrap both for bootx. Any IM4M works once iBSS is patched
IM4M=build/hoolock-docs/binaries/im4m/0x8015.im4m
ipsw img4 create --input build/iboot/DeviceTree.bin --type rdtr \
    --im4m $IM4M --output build/iboot/devicetree.img4
ipsw img4 create --input build/iboot/empty_trustcache.bin --type rtsc \
    --im4m $IM4M --output build/iboot/trustcache.img4
```

## Run

Pack m1n1, the device tree, the kernel and the initramfs into one fake restore
kernelcache:

```sh
IM4M=build/hoolock-docs/binaries/im4m/0x8015.im4m
{ cat build/m1n1/build/m1n1-idevice.macho
  echo 'chosen.bootargs=earlycon console=tty0 ignore_loglevel'
  cat build/t8020-d331p.dtb \
      build/linux/arch/arm64/boot/Image.gz \
      build/initramfs.gz
} > build/iboot/m1n1-linux.macho

ipsw img4 create --input build/iboot/m1n1-linux.macho --type rkrn \
    --compress lzss --extra build/m1n1/build/monitor-stub.macho \
    --im4m $IM4M --output build/iboot/kernelcache.img4
```

Put the phone in DFU on the jig. The exploit fires by itself in about a second.
Move the cable to the PC, then:

```sh
./tools/iboot-boot.sh
```

The script wants `usbliter8ctl` at
`../../3rd-party/usbliter8ra1n-patch/tools/usbliter8ctl`. Set `USBLITER8CTL` to
point somewhere else.

About twenty seconds later the phone is `05ac:4142`, hands the PC
`172.16.42.2` over DHCP, and answers telnet on `172.16.42.1`.
`tools/tsh.py "cmd"` runs one command there from a script.

If the script says the phone is not pwned, check its USB serial for
`PWND:[usbliter8]`. Without it the exploit has not run: unplug the jig from USB
for a few seconds, a firmware reset is not enough, and put the phone back in
DFU.

## Implementation status

- [x] Boot chain, tethered, nothing read off the NAND
  - [x] Patched iBSS that takes an unsigned payload
  - [x] m1n1 as a fake restore kernelcache, with a real BootArgs
  - [ ] Makefile targets for `build/iboot/`, packed by hand today
- [x] Device tree for T8020, PMGR domains generated from the ADT
- [x] Console
  - [x] Framebuffer, handed over by iBoot
  - [ ] Display driver. The picture is frozen as iBoot left it
  - [ ] Notch trimmed out of the framebuffer
- [x] AIC interrupts, including fast IPIs
- [x] SMP on all six cores
  - [ ] Chicken bits reapplied per core by the kernel, m1n1 does it once
- [x] NVMe through ANS2, reads and writes, ext4 mounted
- [x] USB
  - [x] Gadget: telnet, DHCP, getty on `/dev/ttyGS0`
  - [x] DART, in m1n1 and through `apple-dart` in Linux
  - [ ] m1n1's proxy surviving more than one client
- [ ] cpufreq. `apple_soc_cpufreq_module_init` never returns, OPPs untested
- [ ] Touch, wifi, cellular, GPU, audio
- [ ] Anything upstream

## License

`GPL-2.0+ OR MIT`, see [LICENSE](LICENSE). `patches/` keeps the license of the
code it patches: `linux-*` GPL-2.0, `m1n1-*` MIT. Nothing third party is
vendored here. What the boot depends on is listed in [NOTICES.md](NOTICES.md).
