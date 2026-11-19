# Boot

`make boot` does it all. The phone goes from `05ac:1227` (DFU) to `05ac:1281`
(recovery) and takes Apple's recovery protocol. iBSS and iBEC are the same
binary; iBSS stays in the chain since it brings up DRAM and the display.

## Image4 patch

`tools/iboot-image4-patch.py` finds `image4_validate_property_callback`
(0x2bf20) from the `EKEY` literal, walks back to `pacibsp`, and writes
`mov w0, #0; ret`. iBoot returns with `retab`, not `ret`.

A manifest is still needed. From A11 on it is tied to the ECID, so no
`0x8020` IM4M exists; `0x8015.im4m` works as filler.

| patch | manifest | result |
|---|---|---|
| verdict branch at 0x5c530 | none | nothing extracted, crash |
| callback at 0x2bf20 | borrowed | "image not valid" |
| callback at 0x2bf20 | borrowed, plus the fix | boots |

## Payload

| stage | upload as |
|---|---|
| device tree | `rdtr`, then `devicetree` |
| empty trustcache (24 bytes: version 1, a uuid, no entries) | `rtsc`, then `firmware` |
| coprocessor firmware, optional | img4 with the ticket, then `firmware` |
| `m1n1-idevice.macho` plus payload, lzss | `rkrn`, then `bootx` |

`bootx` wants a Mach-O and silently rejects a raw image. Uploading in DFU
breaks the exploit's handler, so `tools/iboot-boot.sh` checks the mode before
each send. It sends `gfx.img4`, `sio.img4` and `aop.img4` from `build/iboot`
when they exist.

## Coprocessor firmware

iBoot loads `gfxf`, `siof`, `aopf` and the rest only with a ticket that lists
them. Borrowed IM4Ms list none, so `make ticket` gets one from Apple's TSS for
this phone. iBoot then adds `segment-ranges` to the ADT node, and m1n1
enables the DT node (`m1n1-0015`).

## Pwned DFU

All on `bmRequestType` 0x21:

| request | does |
|---|---|
| 1 | download, 0x800 bytes at a time, zero length ends it |
| 6 | abort |
| 8 | jump to the image, the device stops answering |

```console
$ cat /sys/bus/usb/devices/*/serial
CPID:8020 ... BDID:1A ... PWND:[usbliter8]
```

## From the NVMe

`make bootbin` builds `boot.bin` (m1n1, bootargs, DT, kernel, initramfs) for
the FAT partition, and `kernelcache-chainload.img4`, m1n1 alone with
`chainload=<PARTUUID>;*`. m1n1 boots the newest `.bin` by FAT time (ties go
to the later name). Volume down opens a menu. The phone's clock is unset, so
`touch -d` a new file. The germin8 jig sends this chain with no PC.

## m1n1 patches

| patch | what |
|---|---|
| 0001 | T8020 USB: complex registers, power domains, USB DART stream 0 |
| 0002 | halt instead of reboot on a fault; clear the SError at the top of RAM |
| 0003 | keep the log on screen with a payload |
| 0004 | proxyclient: an unparsable ADT property is not fatal |
| 0005 | A12 MIDR parts |
| 0007 | dwc2: cache maintenance on DMA buffers, PHY width |
| 0008 | CPU start block at pmgr + 0xd4000 |
| 0009 | A12 chicken bits |
| 0010 | dwc2 errors on the framebuffer |
| 0012 | CPU and GPU rails to software |
| 0013 | PCIe on T8020 |
| 0015 | enable coprocessor nodes iBoot loaded firmware for |
| 0016 | `wcslen`, needed by Rust's core from 1.99 |
| 0017 | T8020 NVMe, SART v1, ascwrap-v2 ASCs, chainload from the NVMe |
| 0018 | boot menu: SMC buttons, large text |

An SError hits every boot at `L2C_ERR_ADR` 0x8ec78c000, cause unknown. Its
status register is write-1-to-clear.
