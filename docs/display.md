# Display

| ADT | address | Linux |
|---|---|---|
| `disp0` | 0x230000000 | `apple-h11p` (`linux-0007`) |
| `mipi-dsim` | 0x230400000 | `adp-mipi` |
| `mipi-dsim/lcd` | | `panel-summit` |
| Acorn display PMU | 0x2a on i2c3 | none |

## Pipe registers

GenPipe starts at 0x50000, 0x8000 per plane.

| offset | what |
|---|---|
| 0x50004 | bit 31 enabled |
| 0x5001c | format word |
| 0x50030 | surface address, low 32 bits |
| 0x50060 | stride |
| 0x50080 | size, `w << 16 \| h` |
| 0x50094 | commit, `0x101` for plane 0 |
| 0x52000 + n*0x808 | gamma tables |

- Writes only land through the mirror at 0x230200000. Writes to 0x230000000
  are dropped.
- No DART. Buffers must be contiguous and in DRAM.

## The driver

`apple-h11p` reads the mode and stride from the pipe, takes the screen from
simpledrm, and flips by writing the surface and committing. It needs
`PD_FLAG_DEV_LINK_ON` on its power domains, or the panel goes black when
simpledrm lets go.

```text
disp0_busif -> disp0_fe -> disp0_be -> mipi_dsi
                                    -> disp0_gp0 -> disp0_gp1 -> disp0_ppp -> disp0_hilo
```

iBoot leaves XRGB2101010. The driver writes the format word on every flip, so
userspace gets XRGB8888.

| word | result |
|---|---|
| 0x03dc8000 | XRGB2101010, iBoot's |
| 0x017c8000 | RGBX8888 |
| 0x057c8000 | XRGB8888 |

Bits 24-25 are the pixel size, 26-27 the channel order, 13-20 a swizzle.

HDR is EDR: the panel has no PQ or HLG mode, so content is drawn below full
code with the brightness turned up. Past 2047 is not found yet.

## Color

iBoot zeroes the color blocks and turns on only WPC, with the ADT's
`primary-calibration-matrix`. Linux colors looked faded next to iOS. The
driver multiplies a 1.4x saturation into that matrix at probe, picked by eye.

| offset | block |
|---|---|
| 0x60000 | blend. 0x60004 = 1 is single layer passthrough |
| 0x64000 | CM. Mode at 0x64004: bit 0 on, bit 3 matrix (0x64008 offsets, 0x64014 3x3, 0x64038 offsets), bit 6 3D LUT at 0x68000, bit 9 1D LUT at 0x70000, bit 11 1D LUT at 0x7240c |
| 0x78000 | WPC. 3x3 at 0x7801c, 16 bit s3.12 |
| 0x7c000 | PRC |
| 0x80000 | dither |
| 0xb0000 | PCC |

- iOS loads a 17x17x17 3D LUT per panel vendor (`color-accuracy-index`) and
  brightness (100 to 650 nits) from tables in the kext. They are close to
  identity on gray.
- WPC takes new values live. Turning on CM mode bit 3 live stalls the pipe on
  solid red until reboot.

## Brightness

```sh
echo 2047 > /sys/class/backlight/230400000.dsi.0/brightness
```

DCS 0x51 with an 11 bit level. Range 139 to 2047, iBoot leaves 740.

## Debugging

- Keep simpledrm on the console while testing.
- A screen that stays lit and frozen after the power button is a blanked CRTC.
  The pipe keeps scanning out; only DCS display off (panel-summit `.disable`)
  makes it dark. Brightness 0 only dims.
- From the m1n1 proxy, power `disp0`, `dart-disp0` and `mipi-dsim` first. Do
  not scan, 0x230100000 faults, and so does part of 0x40000 to 0x50000.
- ramoops does not work: iBSS clears DRAM on every boot.
