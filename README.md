# linux4a12

Mainline Linux bring-up for the Apple A12 (T8020), targeting the iPhone XS Max
(iPhone11,6, board D331pAP).

Goal for this stage is narrow on purpose: a serial console and an initramfs
shell. No display, no touch, no wifi, no cellular, no GPU. That is also as far
as the A10 and A11 ports have got, so it is the honest finish line.

## Why A12 is possible now

Upstream Linux has device trees for A7 through A11 only, because the boot chain
needed checkm8 and checkm8 stops at A11. The usbliter8 SecureROM exploit
(published June 2026) covers A12 and A13, which opens the same door on this
phone. Pre-boot code execution is already solved by other people, so the work
left here is the kernel side.

## Boot chain

    SecureROM
      -> usbliter8 (RP2350 board sends crafted USB setup packets in DFU)
      -> PWND DFU
      -> patched iBSS / iBEC
      -> PongoOS
      -> bootm
      -> m1n1
      -> Linux (16K pages) + t8020-d331p.dtb + initramfs

Nothing is written to the phone. Reboot returns it to iOS.

## Where the numbers come from

The addresses in `dts/t8020.dtsi` are read out of the DeviceTree inside the
iPhone11,6 IPSW, not guessed from T8015. Extract and dump it with:

    ipsw extract --pattern 'DeviceTree.*' -o work <ipsw>
    ipsw dtree --json DeviceTree.d331pap.im4p > refs/iosdt/d331pap.json
    python3 tools/iosdt.py refs/iosdt/d331pap.json uart

`tools/iosdt.py` resolves the `arm-io` ranges property so it prints absolute
addresses. Child 0x0 maps to parent 0x200000000 on this SoC.

Useful findings so far:

| Block   | Address      | Interrupt | Notes                              |
|---------|--------------|-----------|------------------------------------|
| arm-io  | 0x200000000  |           | `arm-io,t8020`, soc-generation H11 |
| uart0   | 0x235200000  | 386       | iBoot `debug-console`, clock gate 76 |
| aic     | 0x23b100000  |           | `aic,1`, same generation as T8015 and M1 |
| pmgr    | 0x23b000000  |           | `pmgr1,t8020`                      |
| wdt     | 0x23d2b0000  | 232       |                                    |
| gpio    | 0x23c100000  | 95 to 101 |                                    |
| cluster 0 (E) | 0x210000000 |     | 4x Tempest, 2 MB L2                |
| cluster 1 (P) | 0x211000000 |     | 2x Vortex, 8 MB L2                 |

CPU affinity differs from T8015: iOS reports `reg` 0 for cpu0 and 256 for cpu4,
so the second cluster sits at affinity level 1, giving MPIDRs 0x0 to 0x3 and
0x100 to 0x101.

The iPhone XS (D321AP) DeviceTree was diffed against this one and the SoC blocks
are identical, so adding that board later is just a second `.dts`.

## Build

    make check

That preprocesses and compiles the device tree using the vendored kernel
headers, so it catches syntax and reference errors without a kernel tree. To
build the real DTB, copy `dts/*` into `arch/arm64/boot/dts/apple/` in the
kernel tree and add the board to that directory's Makefile.

## Expected driver work

Probably none for this stage. The compatible strings fall back to drivers that
already exist upstream:

- `apple,t8020-aic` falls back to `apple,aic`, matched by `irq-apple-aic`
- `apple,s5l-uart` is the existing Samsung driver
- `apple,t8020-wdt` falls back to `apple,wdt`

If that holds, the only kernel change is the two device tree files.

## Not done yet

- m1n1 needs to recognise T8020. Hoolock's fork knows T8015 and earlier.
- Confirm PongoOS from the usbliter8 toolkit still exposes `bootm`.
- pmgr power domains need a generated `t8020-pmgr.dtsi`.
- cpufreq needs measured A12 operating points. Cluster controllers are at
  0x210e20000 and 0x211e20000.
