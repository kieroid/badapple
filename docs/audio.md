# Audio

In progress, not in `patches/` yet. DMA and MCA stream at 48 kHz; the
earpiece stays silent.

| piece | where |
|---|---|
| SIO, DMA coprocessor | 0x236400000, firmware `SmartIOFirmware_ASCv2.im4p` as `siof` |
| MCA | 0x238400000, 6 clusters 0x800 apart; DMA adapters 0x238300000 |
| NCO | 0x23b044000, 5 channels |
| codec cs42l75 | spi3, reset AOP GPIO 20 |
| earpiece amp cs35l27 | i2c2 at 0x40, reset GPIO 136 |
| bottom speaker | behind the AOP |

## SIO

MCA DMA goes through SIO, not ADMAC. Its firmware needs the TSS ticket, see
[boot.md](boot.md). SIO touches the audio, SPI and UART shims from its
interrupt path, so `ps_audio`, `ps_spi_p` and `ps_uart_p` stay on (found by
dr1408 for Project Cascadia).

## MCA

Cluster registers as on M1, 0x800 apart. The selector fields, from iOS
`AppleMCA2Switch`:

| register | field |
|---|---|
| 0x004 bits 2:0 | clock generator source: 0 NCO, 1 + n pin n; enable is 0x000 bit 0 |
| 0x104 | sync generator source: n + 1 clock generator n, 7 + n pin n |
| 0x304, 0x504 bits 18:16 | TX sync select |
| 0x600 bits 3:0 | pin output enables: MCLK, BCLK, LRCK, DOUT |
| 0x604 bits 11:8 | pin clock source |
| 0x608 | pin data source, `1 << 2 * cluster` for TXA |

The earpiece bus is pin 0, clocked by the codec. As iOS sets up `mca2a`
(`syncGen-config` "syn0 pin0"), cluster 0's clock generator takes pin 0's bit
clock, its sync generator runs from that, and cluster 2's TX syncs to it.
Asahi's consumer setup (sync from `BIT(port)`) never moves the DMA. Changing
any of this while a stream runs hard-hangs the phone.

MCLK pins: a port drives its MCLK from PMGR MCK n, which follows NCO n. NCO
enable is bit 31. The codec's MCLK1 is pin 0, the amp's MCLK pin 2, both at
12 MHz. The codec passes MCLK straight through as BCLK, so 12 MHz with a 250
bit frame gives 48 kHz.

## Codec

Apple's SPIv2 framing: `80 a0 a1 a2` address, `81 n0 n1` count, `c6`/`c4`
read bytes or words, `b5`/`b2` write, status polls until `(st & 0x50) ==
0x50` for reads, `& 0x90` for writes. Chip ID 42 a7 50 at 0x0.

| register | what |
|---|---|
| 0x5 | MCLK control, 0 runs the core on MCLK1 |
| 0x10000 | bit 0 power down all, bit 1 PCM bus |
| 0x10009 | ASP SDIN/SDOUT enables, 3 for ASP1 |
| 0x20005, 0x20006 | ASP1 master 0x38, 0x82 |
| 0x410000 + ch, 0x41000a + ch | ASP1 TX slot (bits), config (width, bit 2 enable) |
| 0x440000 + ch, 0x44000a + ch | ASP1 RX slot, config |
| 0x320000 + ch | ASP1 TX source; ASP n RX ch c is 0x1e + 10 (n - 1) + c |
| 0x10001 | CLP control: bit 5 DSP reset, bits 1:0 mode (1 load, 2 ready) |
| 0x2001a | CLP state |

The ADT routes the earpiece through the codec's speaker protection (CLP):
`routing-source_clp_asp_tx` is `59 58 1e 1f ...`, so the amp's slot (bit 16,
`slot-config` 0x10) carries the CLP output `0x58`.

The CLP firmware is `HapticsFirmware` in `Firmware/D331P_CallanFirmware.im4p`
(PRAM0/1 at 0x800000/0x801000, XRAM0/1 at 0x810000/0x811000). The load
sequence follows iOS and the CLP state moves, but every DSP memory access
times out. iOS turns on a second MCLK first, 24 MHz from the AOP (`mckE`).
Switching the codec core to MCLK2 kills SPI even with all six MCA pins
clocked, so MCLK2 is not an MCA pin.

## Amp

CS35L41-style registers from iOS `AppleCS35L27Amp`: block enables 0x2008
(`0x07000d0a`, bit 10 is the PCM path), REFCLK 0x3000 (`0x655`, MCLK at
12 MHz), router 0x7000 (8 is ASP RX1), ASP RX slot 0x7230 and TX slots
0x7210..0x7218 in bits, global enable 0x2000. Status 0x2810 to 0x2824 shows
no faults.
