# cpufreq

E 792 to 1572 MHz, P 744 to 2376 MHz, schedutil. `m1n1-0012` hands the CPU
rails to software, `linux-0011` sets the voltage around every switch.

Cluster DVFS blocks are at 0x210e20000 (E) and 0x211e20000 (P): `CMD` at 0x20
(PS1 bits 3:0, PS2 15:12, `SET` bit 25, `BUSY` bit 31), `STATUS` at 0x50. `CMD`
needs 64 bit writes.

| pstate | E | P |
|---|---|---|
| 1 | 528 MHz | 504 MHz |
| 2 | 792 MHz | 744 MHz |
| 3 | 1056 MHz | 1176 MHz |
| 4 | 1308 MHz | 1704 MHz |
| 5 | 1572 MHz | 2136 MHz |
| 6 | | 2376 MHz |
| 7 | | 2436 MHz, boost |
| 8 | | 2496 MHz, boost |

Frequencies come from the live ADT; the IPSW's are zeroed. iBoot leaves both
clusters at pstate 2.

## Voltage

iBoot leaves the CPU rails ignoring software, so anything above pstate 2
runs on pstate 2's voltage and hangs.

| PMGR | what |
|---|---|
| 0x23b0a0000 | bits 14:8, one per rail: set means software is ignored |
| 0x23b0a000c + 4n | rail n: bits 7:0 the code, bit 31 busy |
| 0x23b0a0020 | P CPU rail |
| 0x23b0a0024 | E CPU rail |

A code is 3.125 mV steps above 375 or 400 mV. Each pstate row (cluster +
0x70000, 0x20 bytes a row) has its code in the first word's top byte. m1n1
clears bits 14:8 and writes each rail's code back. The driver raises the
voltage before the clock and lowers it after.

## Pstate 1

Switching to pstate 1 works, then `BUSY` never clears. Not in the OPP tables.

## Thermal

Two zones, `pcpu-thermal` and `ecpu-thermal`: passive at 85 C, critical at
105 C, cpufreq as the cooling device. A 10 C hysteresis keeps P between 67 and
88 C under full load.

## Boost

Pstate 7 works by hand, 8 reads back as 7. Likely needs cpuidle that powers
a core down. Neither is in the OPP tables.
