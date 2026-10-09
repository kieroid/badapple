# Third party notices

Nothing third party is vendored in this repository. `make deps` and
`make pongoos` clone the following into `build/`, which `.gitignore` keeps out
of the tree, and `patches/` carries the license of whatever it patches. This
file records what the boot chain depends on and under what terms.

| Component | Source | License |
|---|---|---|
| m1n1 | HoolockLinux/m1n1, fork of AsahiLinux/m1n1 | MIT |
| Linux | HoolockLinux/linux | GPL-2.0 |
| Hoolock docs, 16K config, initramfs | HoolockLinux/docs, HoolockRD | see those repos |
| PongoOS | 0cyn/PongoOS branch `t8027`, fork of checkra1n/pongoOS | MIT |
| pico-sdk | raspberrypi/pico-sdk | BSD-3-Clause |
| Arm GNU toolchain | developer.arm.com | GPL with runtime exception |

## PongoOS

The A12 loader is PongoOS. Upstream checkra1n/pongoOS has no T8020 support, so
the fork at `0cyn/PongoOS` branch `t8027` is what gets built, for
`src/drivers/plat/t8020.c` in particular. The fork keeps upstream's MIT
license: `LICENSE.md` is the checkra1n MIT text, and every file it changed
against upstream carries the MIT header.

    The MIT License (MIT)
    Copyright (c) 2019-2023 checkra1n team

## usbliter8

The bootrom exploit, which runs on the RP2350 jig and never on the phone's
side of the boot. See [NOTES.md](NOTES.md#rp2350-firmware) for how it is built
and flashed. `boards/pico2_w.cmake` and `tools/dfuboot.py` are ours.

The copy this was built from carries no license file. That is worth resolving
before anything here is redistributed as a whole.

## l41ka, removed

The jig used to run l41ka, which drove the phone from DFU to a PongoOS shell by
patching iBoot. It is no longer used, for two reasons: it ships no license at
all, and its route goes through iBoot's auto-boot path, which needs a bootable
OS on the phone's flash. Linux now owns that flash. See
[NOTES.md](NOTES.md#why-the-jig-stopped-at-the-exploit).
