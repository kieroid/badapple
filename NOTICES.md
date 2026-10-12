# Third party notices

Nothing third party is vendored here. `make deps` clones the trees below into
`build/`, which `.gitignore` keeps out of the repo, and `patches/` carries the
license of whatever it patches. This file records what the boot depends on and
under what terms.

| Component | Source | License |
|---|---|---|
| m1n1 | HoolockLinux/m1n1, fork of AsahiLinux/m1n1 | MIT |
| Linux | HoolockLinux/linux | GPL-2.0 |
| Hoolock docs, 16K config, IM4M templates, empty trustcache | HoolockLinux/docs | MIT |
| Test initramfs | HoolockLinux/HoolockRD | see that repo |
| `usbliter8ctl`, jig firmware | Trussh/usbliter8ra1n-patch | MIT |
| pico-sdk | raspberrypi/pico-sdk | BSD-3-Clause |
| Arm GNU toolchain | developer.arm.com | GPL with runtime exception |

## usbliter8

The bootrom exploit. It runs on the RP2350 jig and never on the phone's side of
the boot. `usbliter8ctl` from the same tree uploads the patched iBSS over pwned
DFU. See [NOTES.md](NOTES.md#rp2350-firmware) for building and flashing it.
`boards/pico2_w.cmake` is ours.

Trussh/usbliter8ra1n-patch is MIT. The older Trussh/usbliter8 ships no license
file, so nothing here is taken from it.

## Where the img4 pieces come from

`tools/iboot-image4-patch.py` is ours. It finds
`image4_validate_property_callback` by the `EKEY` literal, the way hBootPatcher
(MIT) does, then walks back to the opening `pacibsp` instead of forward to a
`ret`. No code is copied from it.

The component order `tools/iboot-boot.sh` sends, `rdtr` then `rtsc` then
`rkrn`, comes from HoolockLinux/remote_boot (BSD-3-Clause). The IM4M templates
and the 24 byte empty trustcache ship in HoolockLinux/docs under `binaries/`.
README.md generates the trustcache with `printf` instead, so a build needs
neither file.
