# GPU

G11P. Compute shaders run from the m1n1 proxy with `tools/gfx-proxy.py`. No
kernel driver yet.

## Firmware

`Firmware/agx/armfw_g11p.im4p`, a raw RTKit image. iBoot loads it as `gfxf`
only with a manifest that lists it, so `make ticket` fetches one and
`make images` wraps the firmware. iBoot then adds `segment-ranges` to
`/arm-io/gfx-asc`:

| segment | address | size | firmware VA |
|---|---|---|---|
| `__TEXT` | 0x800160000 | 0x44000 | 0 |
| `__DATA` | 0x8001a4000 | 0x8c000 | 0x44000 |

`linux-0019` adds `apple,asc-rtk`, which powers `ps_gfx`, sets RUN and runs the
RTKit handshake. The ASC has no DART.

The firmware cannot be restarted. Each proxy run needs a fresh DFU boot.

## Power

iBSS leaves GPU rails 2 (SRAM) and 4 (core) at 0, so any RGX access wedges the
coprocessor. Clear bits 10 and 12 of 0x23b0a0000 and the rails follow the GPU
DVFS table at 0x23b070000. `m1n1-0012` does this for Linux boots.

## Init block

EP 0x20, `0x81 << 48 | addr`, points at u64s:

| index | what |
|---|---|
| 3 | RegionB |
| 4 | RegionC |
| 5 | firmware mem-desc ring |
| 6 to 0x12 | MMU format |
| 0x13, 0x14 | mem-desc table, count |

Mem-desc entries are `u64 va, u32 pa >> 12, u32 0, u16 16K pages, u16 flags`.

RegionB has 17 channels (12 run queues, device control, event, firmware log,
KTrace, stats), stats at 0x170 to 0x180, hwdata A and B at 0x188 and 0x1a0.

| hwdata B | what |
|---|---|
| 0x0 | VA bases: 0x14 << 32, 0xffc00000, 0x11 << 32, 0x11 << 32, 0x18ffff8000 |
| 0x640 + 0x20 n | IO maps: Fender 0x204d00000, RGX 0x204000000 |
| 0x958, 0x960 | real power mode |
| 0x988 | power sample period, 24 MHz ticks |
| 0xa6c | highest pstate |
| 0xa70 on | perf state tables |
| 0x16d0 | unit masks for TA, 3D, CP: 3, 7, 1 |

| RegionC | what |
|---|---|
| 0x0 | KTrace categories, 0xff for all |
| 0x9150 | per-context TLB generation |
| 0x9158 | per-context page catalogue address |
| 0x9160 on | contexts loaded at every power-up, 32 per word |
| 0xb198 | set by the firmware when init is done |
| 0xb1cc | 1, so the first power-up installs the job handlers |

## MMU

A PowerVR Rogue S7 MMU, registers 0x8ef0 above Rogue's.

| RGX | what |
|---|---|
| 0x17030 | context index |
| 0x17038 | page catalogue address >> 12 |
| 0x17040 | fault status |

16K pages, 40 bit VA:

| level | VA bits | entry |
|---|---|---|
| page catalogue | 39:30 | u32, `(pd >> 12) << 4 \| 1` |
| page directory | 29:21 | u64, `pt \| 3` |
| page table | 20:14 | u64, `page \| 1`, bit 1 read-only |

User VMs use contexts 16 to 63. The low ones belong to the parameter manager:
a job in context 1 faults as "PM client access out of range".

Fault status: VA bits 63:24, context 21:16, requestor 15:8 (3 is CDM), level
6:5, read bit 4, type 3:1.

## Jobs

| what | channel | doorbell |
|---|---|---|
| device control | 12 | `0x83 << 48 \| 0x11` |
| run queue | 0 to 11 | `0x83 << 48 \| channel` |

A compute command (type 3, 0x2e0 bytes):

| offset | what |
|---|---|
| 0x10 | context |
| 0x14 | event control |
| 0x70 | values for RGX 0x14020, 0x14000 (CDM stream), 0x118d0 |
| 0x1f0 | microsequence |
| 0x278 | completion stamp |

Microsequence:

| op | bytes | what |
|---|---|---|
| 0x29 | 0x64 | start compute and kick |
| 0x41 | 4 | wait for the CDM interrupt |
| 0x2a | 0x44 | finalize |
| 0x18 | 4 | end, writes the stamp |

## Compute

CDM block types in bits 31:30: 0 launch, 2 terminate (0x80000000), 3 barrier
(0xc0000160). A direct launch is 8 words:

```text
header
state block address | low bits
global size x, y, z, in threads
threadgroup size x, y, z
```

| header bits | what |
|---|---|
| 0 | always set; at 0 every odd threadgroup is dropped |
| 3:1 | uniform registers, blocks of 32, mod 8 |
| 8:4 | texture states |
| 11:9 | sampler states |

The state block is 64 byte aligned. Its low bits: 5:2 registers in blocks of
8 (0 is 16), 1:0 state block length in 64 bytes.

The state block is a list of words:

```text
u64 3 | first << 9 | count << 21 | va << 24     uniform load, u0 on, up to 31
u64 shader word
u16 0x0108                                      no preshader
  or
u16 0x0038, 48 bits 0xc040 | code << 16         preshader, even address
```

| shader word bits | what |
|---|---|
| 63:38 | code address >> 6 |
| 31:28 | 9 |
| 23:16 | threadgroup memory in 256 bytes, 0 is 64K |
| 8 | uses threadgroup memory |
| 7:4 | registers in blocks of 8, 0 is 16 |
| 3:0 | 0xf |

The uniform address is the full GPU VA. Code addresses are 32 bit offsets from
0x11 << 32. The driver adds 16 bytes to the threadgroup memory size.

## ISA

Close to G13. G11 kernels (GPU Mach-O, cpusubtype 0x72) are in the iOS 16
CoreImage archives and, for iOS 18, also in QuartzCore, ImageIO and RenderBox.
applegpu's G13 disassembler reads nearly all of them.

| | G11 |
|---|---|
| stop | `0x0801` |
| threadgroup load, store | device load, store (`0x05`, `0x45`) with bits 29:28 = 2, bit 26 and bits 46:45 clear, a 16-bit register base holding the byte address |
| threadgroup barrier | `0x0068`, same as G13 |
| memory barrier | only `0xf596` |
| SIMD reductions (`simd_iadd`) | none |

An instruction G11 lacks kills the thread without a fault, and the job still
completes. A store's register offset is scaled by the element size.

## Mesa

`patches/mesa-0001-asahi-g11.patch` makes Mesa's Asahi compiler emit G11
code. `make gfx-tests` builds its `asahi_clc` and compiles
`tools/gfx-tests.cl`, and `tools/gfx-proxy.py clc` runs the result. Kernel
arguments go in uniforms from u0, and rodata is loaded from the binary into
the uniforms Mesa asks for.

These pass: arithmetic, floats, loops, a preamble, thread and threadgroup ids,
threadgroup memory with barriers, a reduction, fences and atomics.

## Queues

Several commands can go through one queue in one firmware session. The run
message is `{type, info, head, queue index, flush}`. The firmware stores head
at info+0x28 and runs the ring from its read index up to it. The next command
goes in the next ring entry with head + 1.
