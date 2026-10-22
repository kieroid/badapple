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

## Brightness

```sh
echo 2047 > /sys/class/backlight/230400000.dsi.0/brightness
```

DCS 0x51 with an 11 bit level. Range 139 to 2047, iBoot leaves 740.

## Debugging

- Keep simpledrm on the console while testing.
- From the m1n1 proxy, power `disp0`, `dart-disp0` and `mipi-dsim` first. Do
  not scan, 0x230100000 faults.
- ramoops does not work: iBSS clears DRAM on every boot.
