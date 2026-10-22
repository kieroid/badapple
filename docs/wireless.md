# PCIe, wifi and Bluetooth

`apcie,t8020` has the T8103 register layout. The tunables are only in the live
ADT, which m1n1 exports as `/dev/mtd1`. `m1n1-0013` sets up the controller.

| port | device | PERST# | CLKREQ# | DART |
|---|---|---|---|---|
| 2 | BCM4377 wifi and Bluetooth | GPIO 106 | GPIO 101 | `dart-apcie2` |
| 3 | baseband | GPIO 107 | GPIO 102 | `dart-apcie3` |

- Port n's PHY glue is at rc + 0x88000 + 0x4000 n, so the DT gives `phy2`.
- `dart-apcie2` has one SID and no RID table. `linux-0012` skips the table
  when it reads back empty.
- Wifi REG_ON is SMC key `gP0e` (`pwren-gpios`, `linux-0012`), Bluetooth power
  `gP09` (a DT hog).

## BCM4377

Revision B2, module USI, island `geniek`. `tools/bcmfw.py` renames iOS
16.6.1's firmware files to what `brcmfmac` and `hci_bcm4377` ask for. m1n1
fills in MAC addresses, antenna SKU and calibration.

## Wifi

- `linux-0014` leaves both DART protection bits off on 4K page DARTs. That
  DART reads `NO_WRITE` as no read, which killed the firmware on its first
  transmit.
- The sysfs firmware fallback is off, or the missing `regulatory.db` stalls
  boot for 60 s.
- Connecting needs `iw` and `wpa_supplicant`, here from Alpine 3.21 with
  `LD_LIBRARY_PATH`, and a `udhcpc` script.
- `CONFIG_BRCMDBG` with `brcmfmac.debug=0x108000` puts the firmware console in
  dmesg.

## Bluetooth

`linux-0013`:

- makes the beamforming blob optional, the 4377 has none
- writes the high word of the context address to BAR2 `0x200490`, not
  `0x200450`
- wakes the chip before the bootstage read: write 1 to BAR0 `0x154`, wait
  25 us, up to four times

Do not read the Bluetooth BARs after a failed probe. The read takes the phone
down.
