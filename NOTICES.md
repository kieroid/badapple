# Third party notices

Nothing third party is vendored here. `make deps` clones the trees below into
`build/`, which git ignores. `patches/` keeps the license of what it patches.

| Component | Source | License |
|---|---|---|
| m1n1 | HoolockLinux/m1n1, fork of AsahiLinux/m1n1 | MIT |
| Linux | HoolockLinux/linux | GPL-2.0 |
| Hoolock docs, 16K config, IM4M templates, empty trustcache | HoolockLinux/docs | MIT |
| Test initramfs | HoolockLinux/HoolockRD | see that repo |
| Jig firmware | Trussh/usbliter8ra1n-patch | MIT |
| Mesa, cloned by `make gfx-tests` | mesa/mesa on freedesktop.org | MIT |

## Ours

- `tools/dfuboot.py`.
- `tools/iboot-image4-patch.py`. It finds the callback by the `EKEY` literal,
  like hBootPatcher (MIT), but no code is copied from it.
- `tools/mtfw.py`. Apple's touch firmware is converted at build time and never
  leaves `build/`.

The older Trussh/usbliter8 has no license file, so nothing is taken from it.
The order `tools/iboot-boot.sh` sends images in comes from
HoolockLinux/remote_boot (BSD-3-Clause).
