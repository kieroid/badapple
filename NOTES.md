# Notes

Registers, gotchas, and things that turned out wrong. Build and boot are in
[README.md](README.md).

## Status

| Device | Boot | Console | Display | XRGB8888 | Brightness | HDR | AIC | SMP | Chicken bits per core | NVMe | Battery | Buttons | Poweroff | Die temp | Thermal zones | USB gadget | Touch | Touch interrupt | Touch power off | Force sensor | cpufreq | Boost | cpuidle | PCIe | Wifi | Bluetooth | Audio | Cellular | Camera | GPU | Upstream |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| iPhone XS Max (D331p) | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :x: | :white_check_mark: | :white_check_mark: | :x: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :x: | :x: | :white_check_mark: | :x: | :x: | :white_check_mark: | :white_check_mark: | :x: | :x: | :x: | :x: | :x: | :x: |

- Wifi joins WPA2 networks on 2.4 and 5 GHz, with Alpine's `iw` and
  `wpa_supplicant` brought in by hand.
- Bluetooth boots its firmware, then never reaches RTI state 2.

## Reading the ADT

```sh
ipsw extract --pattern 'DeviceTree.*' -o work <ipsw>
ipsw img4 im4p extract -o build/adt-d331p.bin \
    work/*/Firmware/all_flash/DeviceTree.d331pap.im4p
cd build/m1n1/proxyclient
../../venv/bin/python -c '
from m1n1.adt import load_adt
adt = load_adt(open("../../adt-d331p.bin", "rb").read())
for c in adt["/arm-io/usb-complex"]:
    print(c._path, [hex(c.get_reg(i)[0]) for i in range(len(c.reg))])'
```

Each `reg` is an offset from its parent, and `/arm-io` adds 0x200000000 through
its `ranges`. Raw `reg` gives the offset. `get_reg()` gives the real address.

## Addresses

| Block | Address | IRQ | Notes |
|---|---|---|---|
| uart0 | 0x235200000 | 386 | iBoot `debug-console` |
| usb-complex | 0x239000000 | | PHY at +0x60 |
| usb-device | 0x239100000 | 451 | dwc2 |
| dart-usb | 0x239900000 | 454 | all three USB controllers |
| pmgr | 0x23b000000 | | `pmgr1,t8020` |
| aic | 0x23b100000 | | |
| gpio | 0x23c100000 | 95 to 101 | 222 pins |
| aop-gpio | 0x24a820000 | 176 to 182 | 52 pins |
| pmgr_mini | 0x23d200000 | | |
| wdt | 0x23d2b0000 | 232 | |
| spmi | 0x23d180700 | | PMU at slave 0xf |
| smc | 0x23e400000 | 253 to 256 | |
| i2c3 | 0x23501c000 | 411 | Acorn display PMU at 0x2a |
| spi1 | 0x235104000 | 396 | touch |
| disp0 | 0x230000000 | | display pipe |
| mipi-dsim | 0x230400000 | | DSI host |
| cluster 0 (E) | 0x210000000 | | 4x Tempest, 2 MB L2 |
| cluster 1 (P) | 0x211000000 | | 2x Vortex, 8 MB L2 |

`dts/t8020-pmgr.dtsi` comes from `make pmgr DT=<DeviceTree im4p>`, not by hand.
The iPhone XS (D321AP) ADT matches this one for the SoC, so it would be a second
`.dts`.

## CPUs

CPU `reg` must be `MPIDR_EL1 & 0xffffff`. The ADT drops Aff2: it says 0 to 3 and
256, 257. The real values are 0x0 to 0x3 (E) and 0x10100, 0x10101 (P). With the
ADT values m1n1 drops every CPU node.

## Console and shell

- **Output.** The framebuffer, as `simple-framebuffer` with `console=tty0`, until
  the display driver takes it over. The node lists four display power domains,
  or they get switched off as unused.
- **Notch.** m1n1 looks for `partially-occluded-display`, which this board does
  not have. `patches/m1n1-0011` adds board 0x1a with a 132 pixel notch, so `fb0`
  is 1242x2556.
- **Input.** USB gadget: telnet on `172.16.42.1`, DHCP gives the host
  `172.16.42.2`, and a getty on `/dev/ttyGS0`. A silent ACM port is normal, the
  getty waits for input first.
- `tools/tsh.py "cmd"` runs one command over telnet.

UART0 works for `earlycon` but needs a cable on the Lightning pins.

## Boot chain

`make boot` runs all of it. `tools/dfuboot.py` sends the patched iBSS over pwned
DFU. The phone goes from `05ac:1227` (DFU) to `05ac:1281` (recovery). From there
it is Apple's own recovery protocol.

iBSS and iBEC are not encrypted, only compressed. They are the same binary here,
so sending iBEC does nothing. iBSS stays in the chain because it brings up the
display.

### The image4 patch

`iboot_patcher.py` patches 0xe10, which is a stack cookie check, not image4. The
real `image4_validate_property_callback` is at 0x2bf20. `tools/iboot-image4-patch.py`
finds it from the `EKEY` literal, walks back to `pacibsp`, and writes
`mov w0, #0; ret`. hBootPatcher walks forward to a `ret` and fails, because
iBoot returns with `retab`.

### A manifest is still needed

| patch | manifest | result |
|---|---|---|
| verdict branch at 0x5c530 | none | accepted, nothing extracted, crash |
| callback at 0x2bf20 | borrowed | "image not valid" |
| callback at 0x2bf20 | borrowed, plus the fix | boots |

No `0x8020` IM4M exists and none can: from A11 on it is tied to the ECID.
`0x8015.im4m` works as filler once the callback always passes.

### The payload

`bootx` wants a Mach-O, so the blob starts with `m1n1-idevice.macho`. A raw
image is rejected with no message. It also needs the stages before it: device
tree as `rdtr` then `devicetree`, an empty trustcache as `rtsc` then
`firmware`, then the payload as `rkrn` (lzss) and `bootx`. The trustcache is 24
bytes: version 1, a uuid, no entries.

Uploading while the phone is still in DFU breaks the exploit's handler.
`tools/iboot-boot.sh` checks the mode before each send.

### Pwned DFU

All on `bmRequestType` 0x21:

| request | does |
|---|---|
| 1 | download, 0x800 bytes at a time, zero length ends it |
| 6 | abort |
| 8 | jump to the image |

The device stops answering during request 8. That is the jump.

### The jig

A Pico 2 W with usbliter8. Plug a DFU phone in and the exploit fires in about a
second. Move the cable to the PC after that.

```sh
cmake -S . -B build -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350
cmake --build build -j
git submodule update --init --depth 1 lib/cyw43-driver   # needed by pico_status_led
```

Needs pico-sdk 2.2.0 or newer and an arm-none-eabi GCC. `boards/pico2_w.cmake`
is ours: Pico 2 pins, no LED, since the W's LED is on the radio. Flash with
BOOTSEL or `picotool load -t elf build/usbliter8.elf -x -f`.

Check the phone is pwned:

```console
$ cat /sys/bus/usb/devices/*/serial
CPID:8020 ... BDID:1A ... PWND:[usbliter8]
```

No `PWND` means the exploit did not run.

## m1n1 patches

| patch | what |
|---|---|
| 0001 | T8020 USB: complex registers, power domains 0x802a8, 0x802b0, 0x802c0, USB DART stream 0 |
| 0002 | halt instead of reboot on a fault, clear the SError properly, limit how many get printed |
| 0003 | keep the log on screen with a payload |
| 0004 | proxyclient: an ADT property it cannot parse is not fatal |
| 0005 | A12 MIDR parts, or m1n1 takes an M4-only path |
| 0007 | dwc2: cache maintenance on DMA buffers, and the PHY width |
| 0008 | CPU start block at pmgr + 0xd4000 |
| 0009 | A12 chicken bits, from XNU's `start.s` |
| 0010 | dwc2 errors to the framebuffer, not only the UART |
| 0011 | notch for board 0x1a |
| 0012 | hand the CPU rails to software, for cpufreq |
| 0013 | PCIe on T8020 |

An SError hits every boot at the top of RAM (`L2C_ERR_ADR` 0x8ec78c000). Cause
unknown. Its status register is write-1-to-clear, so m1n1's usual clear, a
write of zeroes, did nothing.

`0003`, `0004` and `0006` exist because the framebuffer is the only console
without a cable, and a `RELEASE` build silences it when a payload is present.

## USB

### The DART

The gadget used to fail with `device descriptor read/64, error -110`. Device
side, both directions showed `AHBErr`: the controller faulted on its first DMA.

The ADT gives `usb-device` an `iommu-parent`, `dart-usb`, stream 0. iBSS leaves
that DART in bypass, and bypass does not reach DRAM at 0x800000000. m1n1 maps
stream 0. Linux has a `dart_usb` node and `iommus = <&dart_usb 0>` on the gadget.

### Remap widget

Three (offset, value, mask) entries at 0x28, 0x38, 0x48. Only 0x48, the device,
matters. The low byte is spliced into address bits 35:32, and bits 24 and 25 are
the DMA enables for each direction. The value must be 0, since the DART sits
after it and dwc2 is a 32 bit master.

### PHY signal bits

T8020 moves them up by 8 compared to m1n1: cable connected is `BIT(16)`, host
role is `7 << 20`. VBUS detect stays forced. A working PHY reads `0x01c10003`.

### Non-posted writes

dwc2 dropped every write while reads worked. `bus@239000000` needs its own
`nonposted-mmio`: the kernel only checks the node and its parent. The same
mistake in `linux-0001`, a plain `ioremap` of the complex, raised an SError
that only killed E cores. Use `ioremap_np` everywhere on this SoC.

### The first attach fails, worked around

Most boots, the host resets twice and never assigns an address. Device side,
`DOEPINT0` shows an OUT token to a disabled endpoint while `DOEPCTL0` says it is
enabled. Ruled out: the DMA path, PIO, re-priming the SETUP, `hsotg->connected`,
a forced disconnect, m1n1's own gadget, the host port, and unused clocks or
domains.

Pulling the cable always fixes it, so the gadget does the same with
`DCTL_SFTDISCON`, every 3 s, up to 10 times:

| state | action |
|---|---|
| configured | stop |
| address set | wait |
| no address | toggle soft disconnect |

Five unattended boots came up, in 5 to 35 s. `mdelay(50)` in the start path
replaces four `dev_info` calls that used to make it work by accident.

Other timing traps: the cable has to move before iBSS runs, a 100 ms poll loop
on the gadget breaks enumeration, and m1n1 polls USB from its main loop, so
anything busy in m1n1 looks like broken USB.

## AIC

genpd switched off `ps_aic` as unused, because the AIC driver starts before the
driver model and never claims it. After that no interrupt arrives and AIC reads
hang, but the timer is a core FIQ, so the boot still looks fine. Fix:
`apple,always-on` on `ps_aic`.

- `AIC_WHOAMI` is 4 on the boot CPU. `linux-0003` maps it.
- `TARGET_CPU` takes `BIT(AIC_WHOAMI)`, not `BIT(0)`.
- 1024 interrupts, registers at 0x4100 and 0x4180.
- Reading 0x4200 raises an abort.

## SMP

All six cores work, with IPIs. It needs:

- CPU start block at pmgr + 0xd4000 (`m1n1-0008`)
- the real MPIDRs in the DT
- CPU and CPM power domains always-on
- A12 chicken bits (`m1n1-0009`)
- `apple,t8015-aic` first, for fast IPIs

The bug that looked like SMP was the posted write in the USB PHY driver, see
[non-posted writes](#non-posted-writes). It was found with a temporary SError
hook that logged `L2C_ERR_STS` and `L2C_ERR_ADR`. CPU hotplug does not work
with spin-table.

## NVMe

ANS2 with SART and an RTKit mailbox. Reads at about 467 MB/s, writes at 132
MB/s, data survives a remount. Mount with `mount -t ext4 /dev/nvme0n1 /mnt`.

Seven faults, each hiding the next:

1. **iBoot's admin queue stays programmed.** Write `AQA`, `ASQ` and `ACQ` before
   starting the coprocessor.
2. **APCIe has to stay on.** Pin `ps_pcie_ref`, `ps_apcie`, `ps_apcie_gp` and
   `ps_apcie_st`.
3. **NVMMU but no linear SQ.** Split `has_lsq_nvmmu` into two flags.
4. **No completion interrupt.** The AIC affinity fix above.
5. **The SMART log looked fatal.** It was fault 7.
6. **Set Features before Create IO CQ.** T8020 cares, M1 does not.
7. **The NVMMU indexes by ring slot, not tag.** Invalidation still uses the tag.

ADT facts:

| what | address |
|---|---|
| ASC wrapper | 0x277400000 |
| mailbox | 0x277408000, T8015 type, not v4 |
| coprocessor run bit | 0x277400044 bit 4 |
| NVMe registers | 0x27acc0000 |
| SART | 0x27ac50000, `sart-version` 1, same as v2 |

The register at 0x27ad20044 looks like a run bit and does nothing. Interrupts
come in descending pairs (365, 364, 367, 366); read them as (not empty, empty).
NVMe is interrupt 371.

Traps: ANS keeps old crash logs across resets, so compare times. `/dev/mem`
cannot reach MMIO on arm64, use the m1n1 proxy.

## SMC

An RTKit coprocessor like ANS. Upstream drivers cover it.

| | |
|---|---|
| ASC | 0x23e400000 |
| mailbox | ASC + 0x8000 |
| SRAM | 0x23fe00000, 0x50000, from `iop-smc-nub` `region-base` |

Compatible `apple,t8015-smc`. Interrupts are in descending pairs, like ANS.

What works: battery (capacity, health, cycles, voltage, current, temperature),
charger, and all four buttons:

| button | SMC key | event |
|---|---|---|
| side | `bHLD` | `KEY_POWER` |
| volume up | `bVUP` | `KEY_VOLUMEUP` |
| volume down | `bVDN` | `KEY_VOLUMEDOWN` |
| ringer | `bRIN` | `KEY_MUTE` |

The SMC syslog shows each press as `SMC HID Event: 01 <button> <state>`,
buttons 1 to 4 in that order.

### Poweroff and reboot

`macsmc-reboot` needs nvmem cells in the PMU scratchpad, over SPMI. The PMU is
`pmu,imola` at slave 0xf, scratchpad at 0xa000 (`info-leg_scrpad`). Cells:
`boot_stage` at 1, `boot_error_count` at 2 bits 0 to 3, `panic_count` at 2 bits
4 to 7, `shutdown_flag` at 0xf (a whole byte).

`apple,t8015-smc-reboot` has no restart key, so restart uses the watchdog.
`poweroff -f` stays off, `reboot -f` lands in recovery. Without `-f` the test
ramdisk ignores both. `OF: Bad cell count` for the PMIC is harmless.

## Display

| ADT | address | Linux |
|---|---|---|
| `disp0` | 0x230000000 | `apple-h11p` (ours) |
| `mipi-dsim` | 0x230400000 | `adp-mipi` |
| `mipi-dsim/lcd`, `lcd,summit` | | `panel-summit` |
| `i2c3/display-pmu`, Acorn | 0x2a on i2c3 | none |

`adp` does not fit the pipe. The iOS driver is `AppleMobileDispH11P`, the same
IOMFB stack M1 runs on DCP, here on the CPU.

### Pipe registers

From the kext. Three helpers do all register access; name them and a search for
`0x8000 + 0x5` finds every plane register. GenPipe starts at 0x50000, 0x8000
per instance.

| offset | what |
|---|---|
| 0x50004 | bit 31 enabled |
| 0x5001c | format word, see below |
| 0x50030 | surface address, low 32 bits |
| 0x50060 | stride |
| 0x50080 | size, `w << 16 \| h` |
| 0x50094 | commit, `0x101` for plane 0 |
| 0x52000 + n*0x808 | gamma tables |

Live values: enabled, 1242x2688, surface 0xf7cd4000, stride 4992.

**Writes only land through the mirror at 0x230200000.** The window repeats every
0x200000. Reads work through either, writes to 0x230000000 are dropped without
a fault.

**No DART.** Bits 35:32 of the surface come from somewhere before the fetch,
like the USB widget. `dart-disp0` has invalid TTBRs. Buffers must be
contiguous and in DRAM.

### The driver

`linux-0007`, `drivers/gpu/drm/tiny/apple-h11p.c`. It reads the mode and stride
from the pipe, takes the screen from simpledrm, and flips by writing the surface
and committing.

**The panel was black because nothing held the power domains.**
`devm_pm_domain_attach_list(dev, NULL, ...)` makes inactive links. simpledrm
held them with `DL_FLAG_RPM_ACTIVE`, and the driver evicts simpledrm. Fix:
`PD_FLAG_DEV_LINK_ON`. Found by a probe that only showed colour bars with
simpledrm still running.

```text
disp0_busif -> disp0_fe -> disp0_be -> mipi_dsi
                                    -> disp0_gp0 -> disp0_gp1 -> disp0_ppp -> disp0_hilo
```

Two other bugs: no `.fops` in `drm_driver` (oops in `drm_fbdev_dma_fb_open`),
and reprogramming the plane on every damage, which corrupts the picture.

**Pixel format.** iBoot leaves the pipe scanning XRGB2101010. The driver writes
the format word on every flip, so fbdev and userspace get XRGB8888.

The kext builds the word in `FUN_fffffff00901db2c`. Bits 24-25 are the pixel
size, 26-27 the channel order, 13-20 a swizzle of four 2 bit fields. Tested by
drawing red, green, blue and white bands as XRGB8888 and changing the word:

| word | where from | result |
|---|---|---|
| 0x03dc8000 | iBoot | right for XRGB2101010 |
| 0x01dc8000 | iBoot's, size 1 | solid red, an error colour |
| 0x017c8000 | kext, size 1, order 0 | RGBX8888: byte 3 is red, byte 0 unused |
| 0x057c8000 | kext, size 1, order 1 | XRGB8888 |

Bits 21 and 23 differ between iBoot's word and the kext's and are not
understood.

**HDR** here is EDR, as on iOS: the panel has no PQ or HLG mode, so normal
content is drawn below full code and the brightness turned up. Tested with
XRGB2101010, brightness 2047, the left half at code 1023 and the right at 545:
the left is much brighter. iOS goes past 2047 for HDR, probably through a high
brightness mode in `AppleSummitLCD` that is not found yet. The gamma tables at
0x52000 could do tone mapping but are not mapped out.

### Brightness

Upstream `adp-mipi` and `panel-summit`, device tree only.

```sh
echo 2047 > /sys/class/backlight/230400000.dsi.0/brightness
```

The DSI host is a DesignWare core at the start of the window: headers at 0x6c,
payload at 0x70, FIFO status at 0x74. The panel takes DCS 0x51 with an 11 bit
level, low byte first. Range 139 to 2047, iBoot leaves 740. `brightness` reads 0
until written. DWI at 0x23b200000 is for LCD phones and does nothing here.

### Debugging display

- Keep simpledrm on the console while testing, so a crash stays readable.
- From the m1n1 proxy, power `disp0`, `dart-disp0` and `mipi-dsim` first.
  One fault ends the proxy session. Do not scan; 0x230100000 faults.
- ramoops does not work: iBSS reinitialises DRAM on every boot.

## Die temperature

`linux-0006`, `drivers/hwmon/apple-soc-temp.c`, hwmon only.

| sensors | base | registers |
|---|---|---|
| 0 to 4, SoC | 0x23b288000 | 32 bit, 0x4000 apart |
| 5 to 7, P cluster | 0x211e30000 | 64 bit at 0x0, 0x40, 0x200 |
| 8, E cluster | 0x210e30000 | 64 bit at 0x0 |

The ADT names the offsets. The averaging registers read zero, so the driver
uses `readBK`. A read is two 16 bit fields: the low half is the temperature in
1/64 degrees, the high half the raw code. Sensor 2 reads zero and is skipped.

## Touch

Works: 10-finger multitouch on `/dev/input/event1`. `linux-0009` is the driver,
`linux-0010` the SPI fix, `tools/mtfw.py` the firmware converter.

| piece | where |
|---|---|
| controller | `/arm-io/spi1/multi-touch`, `hid-transport,k1`, `hbpp-version` 0x105 |
| SPI | `spi1`, 4 MHz, mode 0, CS on GPIO 67 |
| reset | GPIO 5, active low |
| interrupt | `aop-gpio` 19, low when there is data, group 0 to AIC 176 |
| clock | PMGR 0x23b078000 |
| power | Acorn register 0x05 on i2c3 |
| firmware | `Firmware/D331p_Multitouch.im4p` in the IPSW |

### Power up

1. **Acorn start.** Write 0x80 to register 4 (clears UVLO), then the ADT's
   display-pmu values: `seq_trg` 4 to 0x22, `enable` 9 to 0x05. Without bits 0
   and 3 the chip stays silent. iBoot leaves 0x05 at 0x40.
2. **Power sequence.** Each ADT entry is a name and four bytes: assert ms,
   deassert ms, value at power on, value at power off. So clear the 0x40
   pulldown, set 0x06. Register 0x05 ends at 0x0f.
3. **Clock,** 10 ms later. Non-posted, or the write is dropped:

   ```text
   v = read(0x23b078000)
   write(v & ~BIT(31))
   udelay(8)
   write((v & 0x7ffffc00) | 1 | BIT(19))    bit 19 enable, bits 9:0 divider
   poll until bit 18 clears
   ```

4. **Release reset.** The chip pulls the interrupt line low right away.

iBoot leaves the interrupt pin in mode 0, where its data bit does not follow the
pin. Mode 7 (input, interrupt off) does.

### The SPI block

`spi-1,samsung`, `spi-version` 1: the older Samsung S5L block M1's SPI came
from. Same registers up to 0x4c, nothing from 0x10c up.

| what | where |
|---|---|
| TX FIFO level | `STATUS` bits 10:6, depth 16 |
| RX FIFO level | `STATUS` bits 15:11 |
| clear flags | write 0x40000f to `STATUS` |
| master mode | `CFG` bits 4:3 |
| always set | `CFG` bit 14 |
| LSB first | `CFG` bit 13, same as M1 |
| start | `CTRL` = 0xd |

`linux-0010` adds a polled mode for it to `spi-apple`, picked by
`apple,t8020-spi`. It splits long transfers and scales the timeout. No runtime
PM: after a suspend, `spi1` fails to power back up (`now: 0xa4f`).

### HBPP, the bootloader

Bytes on the wire. 32 bit fields go as two 16 bit halves, low half first, high
byte first: 0x400b0fd4 is `0f d4 40 0b`. Sums are byte sums, high byte first.
Each command is followed by `1a a1` in its own transfer, with CS dropped in
between.

| command | sent | reply |
|---|---|---|
| write | `30 01`, words less one, address, sum of bytes 2 to 7, payload, 32 bit sum | `4b c1` |
| read-modify-write | `1e 33`, address, mask, value, sum of bytes 2 to 13 | `4a d1` |
| read | `1c 73`, address, sum | `4c 39`, value, sum |

The write is `apple_z2`'s except for the length. A read clocks out
`1a a1 18 e1 18 e1 18 e1`. One write carries up to 0x40000 bytes.

### The firmware

The im4p holds an unencrypted IOKit XML plist. Each personality (`C1R1FC1,1`) is
a list of steps for the host: `Binary` and `Property` write data, `ReadModifyWrite`
and `Poll` touch registers, `Config` is for after boot. Properties carry default
calibration, so syscfg is not needed.

`tools/mtfw.py` turns it into 40 ops, 393 KB. `make images` puts it in the
initramfs at `/lib/firmware/apple/d331p-mtfw.bin`, appended inside Hoolock's
gzip stream, since m1n1 keeps only one initramfs. Replay takes 2.6 s.

### HIDSPI

After boot the firmware speaks `applespi`'s packet, 512 bytes:

```text
u8   flags      0x20 from the device, 0x40 request or response, 0x80 empty
u8   device     0 touch (grape), 1 force (luna), 0xd0 info
le16 offset, remaining, length
u8   data[502]
le16 crc        CRC-16/ARC, Linux's crc16()
```

Each transaction is 4 bytes of padding and the packet, full duplex. The host
sends an empty packet when it has nothing. CS needs 200 us of setup, or some
packets come back shifted by a byte. The kext has a status stage (0xd56827ac
ACK, 0x0e4fb139 NAK), but this firmware answers it with the next packet.

Messages inside are Asahi's `spihid_msg_hdr`. The device sends `a0 80 00 00`
when ready. Info requests come back empty. Nothing needs enabling.

**Touches** are type 0x10, report 0x44, on interface 0, in the Touch Bar's Z2
format: finger count at byte 16, 30 byte records from 24, `apple_z2`'s
`struct apple_z2_finger`. State 3 is start, 4 is move. Positions are signed,
in 1/100 mm over the whole panel. Size 6890 by 14910, y inverted.

### Left to do

Power off on remove and suspend, the force sensor, better calibration.

One early probe crashed with the screen flashing. Cause unknown, not seen since.

## cpufreq

Works: E 792 to 1572 MHz, P 744 to 2376 MHz, schedutil. `m1n1-0012` hands the
CPU rails to software and `linux-0011` sets the voltage around every switch.

Cluster DVFS blocks are at 0x210e20000 (E) and 0x211e20000 (P), in the T8103
layout: `CMD` at 0x20 with PS1 in bits 3:0, PS2 in 15:12, `SET` bit 25, `BUSY`
bit 31, and `STATUS` at 0x50. `CMD` ignores 32 bit writes, use `writeq`.

| pstate | E | P |
|---|---|---|
| 1 | 528 MHz | 504 MHz |
| 2 | 792 MHz | 744 MHz |
| 3 | 1056 MHz | 1176 MHz |
| 4 | 1308 MHz | 1704 MHz |
| 5 | 1572 MHz | 2136 MHz |
| 6 | | 2376 MHz |
| 7 | | 2436 MHz, boost |
| 8 | | 2496 MHz, boost |

From a live ADT (`voltage-states1-sram`, `voltage-states5-sram`). The IPSW
copies are zeroed. iBoot leaves both clusters at pstate 2.

### The voltage

iBoot leaves the CPU rails in a mode that does not follow the pstate, so a
switch above pstate 2 runs on pstate 2's voltage and hangs the SoC sooner or
later. The PMGR voltage block at 0x23b0a0000:

| offset | what |
|---|---|
| 0xa0000 | bits 14:8, one per rail: set means software requests are ignored. iBoot leaves 0x023fff00 |
| 0xa000c + 4n | rail n, request and readback. Bits 7:0 the code, bit 8 set by a write, bit 31 busy |
| 0xa0020 | P CPU rail |
| 0xa0024 | E CPU rail |

A code is 3.125 mV above a base of 375 or 400 mV. Each pstate's row in the
cluster's pstate table, 0x20 bytes per row from cluster + 0x70000, carries its
code in the top byte of the first word. On P: 0x3e, 0x3e, 0x4a, 0x6a, 0x8e,
0xb0, 0xe0, 0xe2. The rails read back exactly pstate 2's codes at boot, and
stay there through a switch until software takes over.

`AppleT8020PMGR` hands the rails over in `FUN_fffffff0094104e4`: clear bits
14:8 of 0xa0000, write each rail's current code back, wait for bit 31. m1n1
does the same. The driver then writes the target row's code before raising
the clock, and after the clock has come down.

### Pstate 1

A switch to pstate 1 reaches it, then `BUSY` never clears and no later switch
completes until a reset. Seen on both clusters, most often on P. Its row also
looks different: the third word is 0x3000000000000048, without the 0xb8xx
field the others have. Left out of the OPP tables.

### Not shown to be needed

What went in while chasing the hangs and stays because the tested setup had
it: `SPMI_SOC0` to `2` kept on, one lock across both clusters, and 100 us
after a voltage raise before the switch.

### Thermal

Two zones, `pcpu-thermal` on the ACC sensor and `ecpu-thermal` on ACCe, both
passive at 85 C and critical at 105 C, with cpufreq as the cooling device.
`apple-soc-temp` registers its channels as thermal sensors through
`HWMON_C_REGISTER_TZ`.

The die sensor answers within a poll. Under full load P reads about 90 C at
2376 MHz and 10 C less one step down, so with a 2 C hysteresis step_wise let
go on the next poll and P still reached 93 C. With 10 C it stays between 67
and 88 C over 90 s of full load, stepping between 2376, 2136 and 1704 MHz. E
stays near 65 C. A smoother loop would need power_allocator and per core
power numbers.

### Boost

Pstate 7, 2436 MHz at code 0xe0, works when asked for by hand. A request for
8 reads back as `STATUS` 0x78: the hardware stays at 7. Row 8 has 3 in bits
43:40 of its second word where the others have 0xf, which m1n1 calls the
max load. Linux only ever idles cores with WFI, so both P cores always count
as busy. 8 likely needs cpuidle that powers a core down. Neither is in the
OPP table: 7 is 2.5% over 6.

### Left

- cpuidle with core power down, then boost.
- iOS also copies the target row into row 0 on every switch
  (`FUN_fffffff009411418`) and runs a startup (`FUN_fffffff00940eb18`) that
  sets APSC, PLL relock and more. Neither turned out to be needed.

## PCIe, wifi and Bluetooth

`apcie,t8020` has the T8103 register layout: config, rc, PHY, PHY IP, AXI and
fuse, then three regs per port (port, PHY glue, PHY IP lane). The tunables are
not in the IPSW's ADT, iBoot adds them to the live one: `apcie-axi2af`,
`-common`, `-phy`, `-phy-ip` on the controller and `apcie-config` and
`pcie-rc` per bridge. m1n1 exports the live ADT as `/dev/mtd1`.

`m1n1-0013` initializes it with the T8103 table, no fuse bits, the single
`apcie-phy-ip-tunables` set, and without the Gen3 and Gen4 shadow sets a Gen2
port does not have.

| port | device | PERST# | CLKREQ# | DART |
|---|---|---|---|---|
| 2 | BCM4377 wifi and Bluetooth | GPIO 106 | GPIO 101 | `dart-apcie2` |
| 3 | baseband | GPIO 107 | GPIO 102 | `dart-apcie3` |

- Port n's PHY glue is at rc + 0x88000 + 0x4000 n, one slot past where
  `pcie-apple` assumes, so the DT gives `phy2`. Without it the driver writes
  port 1's glue and the boot hangs.
- `dart-apcie2` has one SID and the port no RID to SID table, so everything on
  the port is SID 0. `linux-0012` skips the table when it reads back empty.
- The chip needs two PMU GPIOs, both SMC `gP` keys: `gP0e`, wifi REG_ON
  (`function-reg_on` of `/amfm`) and `gP09`, Bluetooth power
  (`function-power_enable` of `/arm-io/bluetooth`). `linux-0012` adds a
  `pwren-gpios` to `pcie-apple` for the first, the DT hogs the second.

### BCM4377

PCI revision 3, B2. OTP: module DTCK 5.3, vendor u (USI). Island `geniek`, from
`module-instance` in `/arm-io/wlan`. `tools/bcmfw.py` renames iOS 16.6.1's
`C-4377__s-B2` wifi files and `BCM4377B2` Bluetooth files to what `brcmfmac`
and `hci_bcm4377` ask for. m1n1 fills in the MAC addresses, the antenna SKU
(NA) and the calibration blobs through the `wifi0` and `bluetooth0` aliases.

Wifi firmware 16.20.357.3.2.50.143 runs. The sysfs firmware fallback is off,
or the missing `regulatory.db` holds everything up for 60 s.

The first transmit used to kill the firmware: "H2D DMA data transfer error"
in its console, and a DART fault with status 0x80000010, which is
`READ_FAULT` on a valid PTE. That 4K page DART takes the DART1 `NO_WRITE`
bit as no read, so every buffer mapped to the device only was unreadable to
it. `linux-0014` leaves both protection bits off on 4K page DARTs. Scans
worked before because they only need receive buffers.

To connect, Alpine 3.21's aarch64 `iw`, `wpa_supplicant` and their libraries
run against the initramfs' musl with `LD_LIBRARY_PATH`. busybox `udhcpc`
needs a script, there is none in the initramfs. With them it associates,
completes WPA2, gets a DHCP lease and reaches the internet.

For debugging, `CONFIG_BRCMDBG` with `brcmfmac.debug=0x108000` and
`/sys/kernel/debug/ieee80211/phy0/console_interval` set puts the firmware's
own console in dmesg.

Bluetooth: `hci_bcm4377` wants a beamforming blob the 4377 never uses and
these ADTs do not have, `linux-0013` makes it optional. The firmware then
boots and reaches RTI state 1, moves to bootstage 3 and never acks state 2.
Upstream was written for the T2 Macs' B3.

## Debugging

- simpledrm's console lags. `boot_delay=8` lets it keep up, `initcall_debug`
  shows the last call, `panic=0` keeps a panic on screen.
- `CONFIG_FB_SIMPLE` never binds here.
- `g_multi` errors are harmless; the initramfs makes its own gadget.
- Check the object's timestamp before trusting a test. `$(IMAGE)` depends on
  `FORCE` for this reason.
- When a block works at probe and fails later, check its `ps_*` domain first.
- No m1n1 hypervisor: nothing here hands over at EL2.

## Corrections

- "Any repacked initramfs panics." Wrong. The script ran before busybox made
  `/bin/sh`.
- "Not the USB DART." It was the DART.
- `05ac:12a8` is another phone. Ours is `05ac:4142`.
- "cpufreq needs the CLPC coprocessor." Wrong. The rails were in a mode that
  ignores the pstate.
- "`ioremap_np` hangs the E cluster block." Wrong, that was a pstate 5 hang.
- "P works once, then `BUSY` sticks." Wrong, P went 2 to 3 to 4 every time.

## Known gaps

1. cpufreq: pstate 1 and boost unused, thermal control is coarse.
2. Chicken bits are set once by m1n1, not per core by the kernel.
3. The first USB attach fails; the gadget retries.
4. Touch has no power off and no force sensor.
5. Bluetooth stops before RTI state 2. Wifi needs its tools brought in.
