# Touch

10-finger multitouch on `/dev/input/event1`. `linux-0009` is the driver,
`linux-0010` the SPI mode, `tools/mtfw.py` the firmware converter.

| piece | where |
|---|---|
| controller | `/arm-io/spi1/multi-touch`, `hbpp-version` 0x105 |
| SPI | `spi1`, 4 MHz, mode 0, CS on GPIO 67 |
| reset | GPIO 5, active low |
| interrupt | `aop-gpio` 19, active low |
| clock | PMGR 0x23b078000 |
| power | Acorn register 0x05 on i2c3 |
| firmware | `Firmware/D331p_Multitouch.im4p` |

## Power up

1. Acorn: write 0x80 to register 4, 0x22 to register 4, 0x05 to register 9.
2. Register 0x05: clear the 0x40 pulldown, set 0x06. It ends at 0x0f.
3. Clock, 10 ms later, non-posted:

   ```text
   v = read(0x23b078000)
   write(v & ~BIT(31))
   udelay(8)
   write((v & 0x7ffffc00) | 1 | BIT(19))    bit 19 enable, bits 9:0 divider
   poll until bit 18 clears
   ```

4. Release reset. The interrupt line goes low.

The interrupt pin needs mode 7 (input, interrupt off). iBoot leaves mode 0,
where the data bit does not follow the pin.

## SPI

`spi-1,samsung`, an older block than `spi-apple` drives. Same registers up to
0x4c, nothing from 0x10c up.

| what | where |
|---|---|
| TX FIFO level | `STATUS` bits 10:6, depth 16 |
| RX FIFO level | `STATUS` bits 15:11 |
| clear flags | write 0x40000f to `STATUS` |
| master mode | `CFG` bits 4:3 |
| always set | `CFG` bit 14 |
| start | `CTRL` = 0xd |

`linux-0010` adds a polled mode for it, picked by `apple,t8020-spi`. No runtime
PM: `spi1` does not power back up after a suspend.

## HBPP

The bootloader. 32 bit fields go as two 16 bit halves, low half first, high
byte first: 0x400b0fd4 is `0f d4 40 0b`. Each command is followed by `1a a1` in
its own transfer.

| command | sent | reply |
|---|---|---|
| write | `30 01`, words less one, address, sum, payload, 32 bit sum | `4b c1` |
| read-modify-write | `1e 33`, address, mask, value, sum | `4a d1` |
| read | `1c 73`, address, sum | `4c 39`, value, sum |

## Firmware

The im4p holds a plist of steps: `Binary` and `Property` write data,
`ReadModifyWrite` and `Poll` touch registers. Default calibration is included.
`tools/mtfw.py` turns it into 40 ops, 393 KB, at
`/lib/firmware/apple/d331p-mtfw.bin`. Replay takes 2.6 s.

## HIDSPI

After boot, 512 byte packets, full duplex, 4 bytes of padding before each:

```text
u8   flags      0x20 from the device, 0x40 request or response, 0x80 empty
u8   device     0 touch, 1 force, 0xd0 info
le16 offset, remaining, length
u8   data[502]
le16 crc        CRC-16/ARC
```

CS needs 200 us of setup. The device sends `a0 80 00 00` when ready.

Touches are type 0x10, report 0x44: finger count at byte 16, 30 byte records
from 24. State 3 is start, 4 is move. Positions in 1/100 mm, 6890 by 14910, y
inverted.

## Left

Power off on remove and suspend, the force sensor, calibration.
