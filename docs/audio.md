# Audio

The earpiece plays: `aplay -D hw:0,0`, 48 kHz, 16 bit stereo. Only the left
channel reaches it, both chips stay on from boot and there is no volume
control yet. Patches `linux-0015` to `linux-0017`.

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
| 0x10009 | ASP SDIN/SDOUT enables, 3 << 2 (n - 1) for ASP n |
| 0x20000 + 6 (n - 1) + 5, + 6 | ASP n master 0x38, 0x82 |
| 0x410000, 0x420000, 0x430000 | ASP1..3 TX: + ch slot (bits), + 10 + ch config (width, bit 2 enable), + 0x14 data pins (4) |
| 0x440000, 0x450000, 0x460000 | ASP1..3 RX, same layout |
| 0x320000 + ch | ASP1 TX source; ASP n RX ch c is 0x1e + 10 (n - 1) + c |
| 0x10001 | CLP control: bit 5 DSP reset, bits 1:0 mode (1 load, 2 ready) |
| 0x2001a | CLP state |

Pin 0 is the codec's ASP3. ASP1 is a separate bus from the codec to the amps.
iOS plays in MCA slots 12 and 13, which land on ASP3 RX ch 8 and 9 (slot
times 16 bits). The driver routes ASP3 RX ch 8 straight to ASP1 TX ch 0
(0x320000 = 0x3a), at bit 16 where the amp listens.

iOS instead routes the earpiece through the codec's speaker protection (CLP):
`routing-source_clp_asp_tx` is `59 58 1e 1f ...`, so the amp's slot (bit 16,
`slot-config` 0x10) carries the CLP output `0x58`.

The CLP firmware is `HapticsFirmware` in `Firmware/D331P_CallanFirmware.im4p`
(PRAM0/1 at 0x800000/0x801000, XRAM0/1 at 0x810000/0x811000). It needs a
second MCLK, 24 MHz from the AOP's own MCA (`mckE`), which the AP can start
without the AOP running: PS 0x23d284098 on (`0xf`), 0x23d240008 =
0x80100000, 0x24a870600 = 1. With it the CLP loads and enables (state 0x2001a
= 4). Plain playback does not need it. Never read an AOP block while its
power state is clock gated (0x244): that hangs the phone.

## Amp

CS35L41-style registers from iOS `AppleCS35L27Amp`: block enables 0x2008,
REFCLK 0x3000 (`0x655`, MCLK at 12 MHz), router 0x7000 (8 is ASP RX1), ASP RX
slot 0x7230 and TX slots 0x7210..0x7218 in bits, global enable 0x2000. Status
0x2810 to 0x2824.

Block enables are `0x5300090d` from iOS's default power blocks, plus 0x400
for the PCM path. Bit 0 is the amp itself. The kext's cached default
`0x0700090a` leaves it off and the amp silent with no fault bits.

This part is rev B0 (register 0x4). iOS loads PAC firmware from the kext into
it (0xe08000), but playback works without it. Memory playback (0x6800..,
samples at 0xe08000, trigger 0x2034) plays with no ASP at all, which is how
to test the amp alone.
