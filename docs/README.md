# Notes

Registers, gotchas, and things that turned out wrong.

| Note | Covers |
|---|---|
| [boot.md](boot.md) | boot chain, image4 patch, payload, pwned DFU, m1n1 patches |
| [soc.md](soc.md) | reading the ADT, addresses, CPUs, AIC, SMP, die temperature |
| [usb.md](usb.md) | console, USB gadget, DART, PHY |
| [nvme.md](nvme.md) | NVMe over ANS2 |
| [smc.md](smc.md) | battery, buttons, poweroff |
| [display.md](display.md) | display pipe, pixel format, brightness |
| [touch.md](touch.md) | touch controller, SPI, firmware |
| [cpufreq.md](cpufreq.md) | DVFS, voltage, thermal |
| [wireless.md](wireless.md) | PCIe, wifi, Bluetooth |
| [gpu.md](gpu.md) | GPU coprocessor, MMU, compute |
| [debugging.md](debugging.md) | tips and corrections |

## Status

| Feature | iPhone XS Max (D331p) |
|---|---|
| Boot, console | :white_check_mark: |
| Display, XRGB8888, brightness | :white_check_mark: |
| HDR | :x: |
| AIC, SMP | :white_check_mark: |
| Chicken bits per core | :x: |
| NVMe | :white_check_mark: |
| Battery, buttons, poweroff | :white_check_mark: |
| Die temperature, thermal zones | :white_check_mark: |
| USB gadget | :white_check_mark: |
| Touch | :white_check_mark: |
| Touch interrupt, power off, force sensor | :x: |
| cpufreq | :white_check_mark: |
| Boost, cpuidle | :x: |
| PCIe, wifi, Bluetooth | :white_check_mark: |
| Audio, cellular, camera | :x: |
| GPU | OpenCL kernels from Mesa run from the m1n1 proxy, no driver |

- Wifi joins WPA2 networks on 2.4 and 5 GHz, with Alpine's `iw` and
  `wpa_supplicant` brought in by hand.
- Bluetooth comes up as `hci0` and scans, with Alpine's `hciconfig` and
  `hcitool` brought in by hand.

## Known gaps

1. cpufreq: pstate 1 and boost unused, thermal control is coarse.
2. Chicken bits are set once by m1n1, not per core by the kernel.
3. The first USB attach fails; the gadget retries.
4. Touch has no power off and no force sensor.
5. Wifi and Bluetooth need their tools brought in. Bluetooth pairing is
   untested, it needs `bluetoothd`.
6. GPU: everything runs from `tools/gfx-proxy.py`. No kernel driver yet.
