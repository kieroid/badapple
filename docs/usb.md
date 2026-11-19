# USB and console

## Console

- Output goes to the framebuffer until the display driver takes it over.
- `fb0` is the whole panel, 1242x2688, notch included.
- Input is the USB gadget: telnet on `172.16.42.1`, DHCP gives the host
  `172.16.42.2`, and a getty on `/dev/ttyGS0` that waits for input.
- UART0 works for `earlycon` with a cable on the Lightning pins.

## The DART

`usb-device` is behind `dart-usb`, stream 0. iBSS leaves it in bypass, which
does not reach DRAM at 0x800000000, so the first DMA faults (`AHBErr`). m1n1
maps stream 0, and Linux has `iommus = <&dart_usb 0>`.

## Remap widget

Entries at 0x28, 0x38, 0x48, only 0x48 matters. The low byte is spliced into
address bits 35:32, bits 24 and 25 enable DMA. The value must be 0.

## PHY

Signal bits are 8 higher than on M1: cable connected is `BIT(16)`, host role
`7 << 20`. A working PHY reads `0x01c10003`.

## Non-posted writes

`bus@239000000` needs its own `nonposted-mmio`, or dwc2 drops every write. A
plain `ioremap` raised an SError that killed the E cores. Use `ioremap_np`
everywhere on this SoC.

## First attach

The host often resets twice and never assigns an address. Replugging fixes
it, so the gadget toggles `DCTL_SFTDISCON` every 3 s, up to 10 times, until
it has one. Move the cable before iBSS runs, do not poll the gadget, and note
that anything busy in m1n1 looks like broken USB.
