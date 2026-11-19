# AOP

The always-on processor runs the motion and proximity sensors (`accel`,
`gyro`, `compass`, `pressure`, `prox` under `iop-aop-nub`) and, for the codec,
a 24 MHz MCLK. In progress.

## Firmware

`Firmware/AOP/aopfw-iphone11aop.im4p`, sent as `aop.img4` with the TSS
ticket. iBoot marks the nub `pre-loaded` and adds `segment-ranges`
(physical, DART address, size):

| segment | where | DART address |
|---|---|---|
| `__TEXT` | AOP SRAM 0x24ae00000 | 0x1000000 |
| `__DATA` | AOP SRAM 0x24aebe000 | 0x10be000 |
| `__ETEXT` | DRAM | 0x116b000 |
| `__OS_LOG` | DRAM | 0xfd000000 |

The CPU is left stopped.

## Registers

| what | where |
|---|---|
| ASC | 0x24a400000, CPU control +0x44, T8015 mailbox at +0x8000 |
| firmware window | 0x24a050000, `0x4ae00001`: CPU address 0 is the SRAM |
| `dart-aop` | 0x24ac10000, `dart,t8020`, streams 0 and 15, no power gates |

iOS `AppleA7IOP` starts it with `+0x4000` and `+0xc00` = 0x1111, `+0x4004`
and `+0xc04` = 1 then 0x1000, `+0x4020` bit 0, then CPU control bit 4.
Before that it asks the DART mapper to go active. On A12 the PPL programs
DARTs (`pmap_iommu_ioctl` 0x8028, 0x8027) and can lock their registers.

`dart-aop` reads all zeros from the AP and the first write hard-hangs it, so
the AOP cannot reach its DRAM segments yet.
