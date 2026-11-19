# GPU

G11P. `linux-0020` boots the firmware and registers a DRM render node.
`make rootfs` builds Mesa for it.

| Note | Covers |
|---|---|
| this file | firmware, power, MMU, jobs, kernel driver |
| [gpu-shaders.md](gpu-shaders.md) | compute, ISA, Mesa state, images |
| [gpu-graphics.md](gpu-graphics.md) | graphics state, tilebuffer, tiling heap |
| [gpu-vulkan.md](gpu-vulkan.md) | Honeykrisp, running the tests |

## Firmware

`Firmware/agx/armfw_g11p.im4p`, a raw RTKit image, loaded by iBoot as `gfxf`
with the TSS ticket (`make ticket`, `make images`). iBoot adds
`segment-ranges` to `/arm-io/gfx-asc`:

| segment | address | size | firmware VA |
|---|---|---|---|
| `__TEXT` | 0x800160000 | 0x44000 | 0 |
| `__DATA` | 0x8001a4000 | 0x8c000 | 0x44000 |

The driver powers `ps_gfx`, sets RUN, does the RTKit handshake and starts
endpoints 0x20 and 0x21. The ASC has no DART. Only the firmware maps the RGX
and Fender ranges in the node. The firmware cannot restart: each proxy run
needs a fresh DFU boot.

## Power

iBSS leaves GPU rails 2 (SRAM) and 4 (core) at 0, so any RGX access wedges
the coprocessor. Clearing bits 10 and 12 of 0x23b0a0000 makes the rails
follow the GPU DVFS table at 0x23b070000. `m1n1-0012` does this.

Nothing throttles for heat. The driver pins the firmware's thermal and PPM
limits: a low limit duty-cycles the GPU, 0 parks every job. 1098 MHz gets
hot, so the top pstate defaults to 4 (740 MHz). `agx.max_pstate=6` lifts it.

## Init block

EP 0x20, `0x81 << 48 | addr`, points at u64s:

| index | what |
|---|---|
| 3 | RegionB |
| 4 | RegionC |
| 5 | firmware mem-desc ring |
| 6 to 0x12 | MMU format |
| 0x13, 0x14 | mem-desc table, count |

Mem-desc entry: `u64 va, u32 pa >> 12, u32 0, u16 16K pages, u16 flags`.

RegionB has 17 channels (12 run queues, device control, event, firmware log,
KTrace, stats). Stats at 0x170 to 0x180, hwdata A and B at 0x188 and 0x1a0.

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
| 0x8970 | keep the scheduler polling while a barrier waits |
| 0x9150 | per-context TLB generation |
| 0x9158 | per-context page catalogue address |
| 0x9160 on | contexts loaded at every power-up, 32 per word |
| 0xb198 | set by the firmware when init is done |
| 0xb1cc | 1, so the first power-up installs the job handlers |

## MMU

PowerVR Rogue S7, registers 0x8ef0 above Rogue's.

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
a job in context 1 faults with "PM client access out of range".

Fault status: VA 63:24, context 21:16, requestor 15:8 (3 is CDM), level 6:5,
read 4, type 3:1.

## Jobs

| what | channel | doorbell |
|---|---|---|
| device control | 12 | `0x83 << 48 \| 0x11` |
| run queue | 0 to 11 | `0x83 << 48 \| channel` |

Compute command (type 3, 0x2e0 bytes):

| offset | what |
|---|---|
| 0x10 | context |
| 0x14 | event control |
| 0x70 | values for RGX 0x14020, 0x14000 (CDM stream), 0x118d0 |
| 0x1f0 | microsequence |
| 0x278 | completion stamp |

| microsequence op | bytes | what |
|---|---|---|
| 0x29 | 0x64 | start compute, kick |
| 0x41 | 4 | wait for the CDM interrupt |
| 0x2a | 0x44 | finalize |
| 0x18 | 4 | end, write the stamp |

Several commands can share one queue in one session. The run message is
`{type, info, head, queue index, flush}`. The firmware stores head at
info+0x28 and runs the ring up to it. The next command goes in the next entry
with head + 1.

## DRM

`drivers/gpu/drm/agx`, a render node with the Asahi uAPI
(`include/uapi/drm/asahi_drm.h`).

| | |
|---|---|
| VMs | one MMU context each, 16 to 63 |
| VA | 16K to 0x80_0000_0000, 16K pages |
| `usc_exec_base` | must be 0x11 << 32 |
| submit | async, one `drm_sched` job per compute or render command |
| name | `asahi`, so Mesa picks it up |
| imports | dma-bufs mapped for the GPU, for scanout |
| binds | a bind over existing mappings replaces them and splits what it cuts |
| timestamps | reported in ns, not written |
| ignored | sampler heaps |
| missing | helper programs, more than one render in flight |

Each user context gets a page catalogue at init, and the firmware loads it at
every power-up, so a VM only fills its page tables. After a change the driver
sets the context's TLB generation to the firmware's copy plus 1 << 22, and the
firmware flushes before the next job. Each VM reserves a zeroed page for RGX
0x14020.

Each queue is an entity on a scheduler that runs one job at a time, so a
submit's commands run in order. In-syncs gate the first command, out-syncs
get the last one's fence. A finished job writes its stamp and sends 0x42 on
endpoint 0x21. The firmware cannot reset, so a timed out job wedges the GPU
until reboot; the driver then prints the fault status. Unbinds wait until the
VM is idle.

A render queues its 3D job with the TA job, behind a barrier on the TA stamp
that the firmware resolves itself (glmark2 152 to 289 at 740 MHz). Render
stamps count up by 0x100: bits 14:8 are the job's slot in the event table,
which must come in order. The linking barrier type never released the 3D
job, so the barrier polls.
