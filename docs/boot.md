# Boot

`make boot` runs all of it. The phone goes from `05ac:1227` (DFU) to `05ac:1281`
(recovery), then takes Apple's recovery protocol.

iBSS and iBEC are the same binary and only compressed. iBSS stays in the chain
because it brings up DRAM and the display.

## The image4 patch

`tools/iboot-image4-patch.py` finds `image4_validate_property_callback`
(0x2bf20) from the `EKEY` literal, walks back to `pacibsp`, and writes
`mov w0, #0; ret`. iBoot returns with `retab`, not `ret`.

A manifest is still needed. No `0x8020` IM4M exists, since from A11 on it is
tied to the ECID. `0x8015.im4m` works as filler.

| patch | manifest | result |
|---|---|---|
| verdict branch at 0x5c530 | none | nothing extracted, crash |
| callback at 0x2bf20 | borrowed | "image not valid" |
| callback at 0x2bf20 | borrowed, plus the fix | boots |

## The payload

| stage | upload as |
|---|---|
| device tree | `rdtr`, then `devicetree` |
| empty trustcache (24 bytes: version 1, a uuid, no entries) | `rtsc`, then `firmware` |
| `m1n1-idevice.macho` plus payload, lzss | `rkrn`, then `bootx` |

`bootx` wants a Mach-O and rejects a raw image without a message. Uploading in
DFU breaks the exploit's handler, so `tools/iboot-boot.sh` checks the mode
before each send.

## Pwned DFU

All on `bmRequestType` 0x21:

| request | does |
|---|---|
| 1 | download, 0x800 bytes at a time, zero length ends it |
| 6 | abort |
| 8 | jump to the image, the device stops answering |

Check the phone is pwned:

```console
$ cat /sys/bus/usb/devices/*/serial
CPID:8020 ... BDID:1A ... PWND:[usbliter8]
```

## m1n1 patches

| patch | what |
|---|---|
| 0001 | T8020 USB: complex registers, power domains, USB DART stream 0 |
| 0002 | halt instead of reboot on a fault; clear the SError at the top of RAM |
| 0003 | keep the log on screen with a payload |
| 0004 | proxyclient: an ADT property it cannot parse is not fatal |
| 0005 | A12 MIDR parts |
| 0007 | dwc2: cache maintenance on DMA buffers, PHY width |
| 0008 | CPU start block at pmgr + 0xd4000 |
| 0009 | A12 chicken bits |
| 0010 | dwc2 errors on the framebuffer |
| 0011 | notch for board 0x1a |
| 0012 | hand the CPU and GPU rails to software |
| 0013 | PCIe on T8020 |
| 0015 | enable `apple,asc-rtk` nodes when iBoot loaded their firmware |

An SError hits every boot at `L2C_ERR_ADR` 0x8ec78c000, cause unknown. Its
status register is write-1-to-clear.
