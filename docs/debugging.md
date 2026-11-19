# Debugging

- simpledrm's console lags. `boot_delay=8` helps, `initcall_debug` shows the
  last call, `panic=0` keeps a panic on screen.
- `CONFIG_FB_SIMPLE` never binds here.
- `g_multi` errors are harmless.
- Check the build's timestamp before trusting a test.
- When a block works at probe and fails later, check its `ps_*` domain.
- `/dev/mem` reads MMIO of powered blocks. Writes to a running MCA or an
  unconfigured DART hard-hang the phone.
- iBoot ignoring `bootx`, or uploads failing with 255: try another USB port.
- No m1n1 hypervisor: nothing hands over at EL2.

## Corrections

| claim | truth |
|---|---|
| any repacked initramfs panics | the script ran before busybox made `/bin/sh` |
| not the USB DART | it was the DART |
| the phone is `05ac:12a8` | it is `05ac:4142` |
| cpufreq needs the CLPC coprocessor | the rails ignored the pstate |
| `ioremap_np` hangs the E cluster | it was a pstate 5 hang |
| SMP is broken | a posted write in the USB PHY driver |
| the GPU page tables are wrong | the job ran in a parameter manager context |
| the T8020 NCO enable is bit 12 | bit 31, as on M1 |
