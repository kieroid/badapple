# SMC

An RTKit coprocessor, covered by upstream drivers. Compatible
`apple,t8015-smc`.

| | |
|---|---|
| ASC | 0x23e400000 |
| mailbox | ASC + 0x8000 |
| SRAM | 0x23fe00000, 0x50000 |

Battery, charger and all four buttons work:

| button | SMC key | event |
|---|---|---|
| side | `bHLD` | `KEY_POWER` |
| volume up | `bVUP` | `KEY_VOLUMEUP` |
| volume down | `bVDN` | `KEY_VOLUMEDOWN` |
| ringer | `bRIN` | `KEY_MUTE` |

## Poweroff and reboot

`macsmc-reboot` uses nvmem cells in the PMU scratchpad, over SPMI: PMU at slave
0xf, scratchpad at 0xa000.

| cell | where |
|---|---|
| `boot_stage` | 1 |
| `boot_error_count` | 2, bits 0 to 3 |
| `panic_count` | 2, bits 4 to 7 |
| `shutdown_flag` | 0xf |

Restart uses the watchdog. `poweroff -f` stays off, `reboot -f` lands in
recovery.
