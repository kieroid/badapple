# USB and console

## Console

- Output goes to the framebuffer until the display driver takes it over.
- `m1n1-0011` adds a 132 pixel notch, so `fb0` is 1242x2556.
- Input is the USB gadget: telnet on `172.16.42.1`, DHCP gives the host
  `172.16.42.2`, and a getty on `/dev/ttyGS0` that waits for input.
- UART0 works for `earlycon` with a cable on the Lightning pins.

## The DART

`usb-device` sits behind `dart-usb`, stream 0. iBSS leaves it in bypass, which
does not reach DRAM at 0x800000000. Without a mapping the controller faults on
its first DMA (`AHBErr`). m1n1 maps stream 0, and Linux has
`iommus = <&dart_usb 0>`.

## Remap widget

Entries at 0x28, 0x38, 0x48, only 0x48 matters. The low byte is spliced into
address bits 35:32, bits 24 and 25 enable DMA. The value must be 0.

## PHY

Signal bits are 8 higher than on M1: cable connected is `BIT(16)`, host role
`7 << 20`. A working PHY reads `0x01c10003`.

## Non-posted writes

`bus@239000000` needs its own `nonposted-mmio`, or dwc2 drops every write. A
plain `ioremap` here raised an SError that killed the E cores. Use
`ioremap_np` everywhere on this SoC.

## First attach

Most boots the host resets twice and never assigns an address. Pulling the
cable fixes it, so the gadget toggles `DCTL_SFTDISCON` every 3 s, up to 10
times, until it has an address.

Timing traps: move the cable before iBSS runs, do not poll the gadget, and
anything busy in m1n1 looks like broken USB.
