# Notes

Registers, gotchas, and things that turned out wrong.

| Note | Covers |
|---|---|
| [boot.md](boot.md) | boot chain, image4 patch, pwned DFU, NVMe boot, m1n1 patches |
| [soc.md](soc.md) | ADT, addresses, CPUs, AIC, SMP, die temperature, suspend |
| [usb.md](usb.md) | console, USB gadget, DART, PHY |
| [nvme.md](nvme.md) | NVMe over ANS2 |
| [smc.md](smc.md) | battery, buttons, poweroff |
| [display.md](display.md) | display pipe, formats, color, brightness, notch |
| [touch.md](touch.md) | touch controller, SPI, firmware |
| [cpufreq.md](cpufreq.md) | DVFS, voltage, thermal |
| [wireless.md](wireless.md) | PCIe, wifi, Bluetooth |
| [gpu.md](gpu.md) | GPU firmware, MMU, jobs, kernel driver |
| [gpu-shaders.md](gpu-shaders.md) | compute, ISA, Mesa state, images |
| [gpu-graphics.md](gpu-graphics.md) | graphics state, tilebuffer, tiling heap |
| [gpu-vulkan.md](gpu-vulkan.md) | Honeykrisp, running the tests |
| [audio.md](audio.md) | SIO, MCA, codec, earpiece amp |
| [aop.md](aop.md) | always-on coprocessor, sensors (in progress) |
| [debugging.md](debugging.md) | tips and corrections |

## postmarketOS

Plasma Mobile or phosh on the NVMe, with our kernel and Mesa. Needs
`pmbootstrap` and sudo. `build.sh` takes `plasma` (the default) or `phosh`.

```sh
make rootfs && tools/pmos/build.sh plasma
make bootbin BOOTARGS="earlycon console=tty0 ignore_loglevel cma=64M hl_rd=nvme"
```

Boot the plain ramdisk with `make boot`, serve the repo with
`python3 -m http.server 8003 --bind 172.16.42.2`, then:

```sh
tools/tsh.py "wget -qO- http://172.16.42.2:8003/tools/pmos/install.sh | sh"
```

This erases `nvme0n1p2` and puts `boot.bin` on the FAT partition. Then tap
BOOTSEL on the jig to boot pmOS with no PC, or from a PC run
`tools/iboot-boot.sh build/iboot/kernelcache-chainload.img4` after the
exploit. Hold volume down at boot for a menu of the `.bin` files: volume
buttons move, the side button boots.

The user is `user`, password `147147`.

## Status

| Feature | iPhone XS Max (D331p) |
|---|---|
| Boot, console, NVMe boot with a menu | :white_check_mark: |
| Display, XRGB8888, brightness | :white_check_mark: |
| HDR | :x: |
| AIC, SMP | :white_check_mark: |
| Chicken bits per core | :x: |
| NVMe | :white_check_mark: |
| Battery, buttons, poweroff | :white_check_mark: |
| Die temperature, thermal zones | :white_check_mark: |
| s2idle | :white_check_mark: |
| USB gadget | :white_check_mark: |
| Touch | :white_check_mark: |
| Touch interrupt, power off, force sensor | :x: |
| cpufreq | :white_check_mark: |
| Boost, cpuidle | :x: |
| PCIe, wifi, Bluetooth | :white_check_mark: |
| GPU compute, OpenCL through Rusticl | :white_check_mark: |
| GPU GLES 3.0 (dEQP-GLES3 44523 pass), kmscube, glmark2, weston, phosh, Plasma Mobile | :white_check_mark: |
| GPU Vulkan through Honeykrisp, vkcube under weston | :white_check_mark: |
| GPU tilebuffer, Vulkan MSAA and descriptor indexing | :x: |
| Audio DMA (SIO), MCA at 48 kHz | :white_check_mark: |
| Earpiece sound | :white_check_mark: |
| AOP sensors, proximity | :x: |
| Camera, cellular | :x: |

## Known gaps

1. cpufreq: pstate 1 and boost unused, coarse thermal control.
2. Chicken bits are set once by m1n1, not per core.
3. The first USB attach fails; the gadget retries.
4. Touch has no power off and no force sensor.
5. Bluetooth pairing is untested.
6. GPU render targets live in memory (tilebuffer stores unknown), so blending
   and framebuffer reads start new passes. Vulkan has constant descriptor
   indices only and no MSAA.
7. The earpiece is mono with software volume only. The bottom speaker is
   behind the AOP. See [audio.md](audio.md).
8. The AOP firmware loads, but the AP cannot reach its DART yet. See
   [aop.md](aop.md).
