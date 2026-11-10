# GPU

G11P. `linux-0020` boots the firmware and registers a DRM render node.
`make rootfs` builds Mesa for it; `cl-test` and `gl-test` run on the phone.

## Firmware

`Firmware/agx/armfw_g11p.im4p`, a raw RTKit image. iBoot loads it as `gfxf`
only with a manifest that lists it, so `make ticket` fetches one and
`make images` wraps the firmware. iBoot then adds `segment-ranges` to
`/arm-io/gfx-asc`:

| segment | address | size | firmware VA |
|---|---|---|---|
| `__TEXT` | 0x800160000 | 0x44000 | 0 |
| `__DATA` | 0x8001a4000 | 0x8c000 | 0x44000 |

`linux-0020` (`apple,t8020-agx`) powers `ps_gfx`, sets RUN, runs the RTKit
handshake and starts endpoints 0x20 and 0x21. The ASC has no DART. The node
also carries the RGX and Fender ranges, which only the firmware maps.

The firmware cannot be restarted. Each proxy run needs a fresh DFU boot.

## Power

iBSS leaves GPU rails 2 (SRAM) and 4 (core) at 0, so any RGX access wedges the
coprocessor. Clear bits 10 and 12 of 0x23b0a0000 and the rails follow the GPU
DVFS table at 0x23b070000. `m1n1-0012` does this for Linux boots.

Nothing throttles the GPU for heat: the driver pins the firmware's thermal and
PPM limits, since a low limit duty-cycles the GPU and 0 parks every job. At
1098 MHz a long run makes the phone hot, so the top pstate defaults to 4,
740 MHz. `agx.max_pstate=6` lifts it.

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
| flush memory to texture | none, the thread stops there; Mesa leaves it out |
| SIMD reductions (`simd_iadd`) | none |

An instruction G11 lacks kills the thread without a fault, and the job still
completes. A store's register offset is scaled by the element size.

## Mesa

`patches/mesa-0001-asahi-g11.patch` makes Mesa's Asahi compiler and Gallium
driver target G11, so OpenCL runs through Rusticl and GLES through EGL. `AGX_G11` is in
`genxml/agx_g11.h`.

| state word | G11 |
|---|---|
| uniform | tag 0x03 |
| shader | one word for shader, registers and threadgroup memory |
| no preshader | 0x0108 |
| preshader | 0x38, then 0xc04000, then the code address. Fragment preshaders go stale: over a run of draws some cores shade with an earlier draw's results, so Mesa leaves fragment shaders without one. A preshader that samples drops a triangle now and then, so shaders with textures get none either |
| texture | `0x17 \| start << 8 \| count << 20 \| address >> 2 << 26`, count 0 is 64 |
| sampler | `0x13 \| start << 8 \| count << 20 \| address >> 2 << 26` |
| CDM header | bit 0 set; uniforms, textures in 8s, samplers in 4s |

Other G11 differences, from Apple's G11 shaders and the CTS:

| what | G11 |
|---|---|
| sqrt | no srsqrt: x * rsqrt(x), with 0 and inf passed through |
| rounding | floor only: ceil, trunc and roundeven are built from it |
| SIMD groups | none, so no subgroups |
| half denormals | flushed in every f16 source, compares and 16-bit constants included |
| threadgroup atomics | the device atomic with address space 2 in bits 29:28 |
| thread index in threadgroup | built from position and size; Apple's code never reads it |
| stack | G13's stack instructions kill the thread |
| uniforms | 256 halves; how u256 and up are encoded is unknown, so Mesa promotes no further |

Private memory lives in a global buffer allocated per launch, indexed from
the grid position. Graphics stages have none, so Mesa keeps their arrays in
registers. Register spills go to the top of a full 32 KB threadgroup
window, one slice per thread from r3l, which caps the threadgroup size.

### Images

Layouts are from AGXMetalA12 and from Apple's G11 kernels lined up against the
same kernels built for G13 (iPhone13,2 IPSWs for iOS 16.6.1 and 18.6.2).

| what | G11 |
|---|---|
| texture, PBE descriptors | as G13, but word 0 from bit 3 up sits one bit lower. Texture bit 63 (PBE 51) is set when the texture has more than one level, and the sample count sits at 65:64 (PBE 57:56). Texture word 1 bit 38 (G13's mipmapped bit) says a companion buffer follows in word 2 |
| sampler | as G13, 8 bytes |
| sample | 10 bytes, `0x21`, `0xa1` for 3D and arrays: destination halves 15:10, texture 19:16, sampler 23:20, 32-bit destination 24, coordinates 29:25, mask 35:32, LOD mode 42:40, LOD 55:48, dimension 9:8, bit 77 set. The tail's low byte is the offset/compare register, not high register bits: none of Apple's samples go above r21, and a destination at r32 and up reads garbage, so Mesa keeps shaders with samples or image writes in r0-r31 when they fit. 4-bit sampler index: 16 samplers, so with Mesa's txf sampler at 0 a shader gets 15, the last shared with texelFetch above level 0 |
| image write | 8 bytes, `0x31`: data halves 15:10, coordinate halves 19:16 and 39:36, state 23:20 and 35:32, dimension 9:8 as for samples. Arrays: `0xb1`, bit 63 and a 2-byte tail with `0x30` in bits 69:68, 10 bytes. Without it an array write is dropped. With it the write lands but the thread stops there, so nothing after it runs |
| sample offsets, compare | G13's packed offsets or depth reference in a register at 71:64 in place of the high register bits, bit 78 for offsets (as in Apple's kernels), 79 for compare (found on the phone) |
| texture_load | none; Mesa samples at the pixel centre instead. That sampler takes pixel coordinates, which ignore mip levels, so texelFetch above level 0 samples the texel centre in normalized coordinates with an explicit 16-bit LOD through a second internal sampler, the last of the 16 |
| bindless textures | none; descriptors are found from the state index |
| twiddled tiles | G13's sizes, but upright for 2, 8 and 32 byte formats: 64x128, 32x64, 16x32 (`Texture<G11>::sumLevelSizesTiled`). Z order starting with Y, extra bits of the long side on top |
| layout | images are linear, 2D arrays included |

CTS kernel_image_methods, samplerless_reads, cl_read_write_images and
cl_fill_images pass. Still wrong:

- twiddled writes of 8-byte formats from 128x64 up, seen before ail had the
  upright tiles and not checked since (unused, images are linear);
- linear filtering: only 16-bit unorm is within CTS tolerance. 8-bit, snorm,
  half and sRGB filter with about 1/32 weights, and float32 does not filter.
  The sampler has no precision bit.

Metal's own samplers load indirectly, `0x07 | start << 8 | count << 20 |
index << 27`, and the index is not understood. A bad sampler load breaks
every later texture access until the next boot.

### Running

`make rootfs` cross-builds Mesa and `tools/cl-test.c` into an Alpine rootfs;
`tools/rootfs-push.sh` copies it to a tmpfs on the phone:

```sh
make rootfs
./tools/rootfs-push.sh
build/venv/bin/python tools/tsh.py "chroot /mnt/a env RUSTICL_ENABLE=asahi cl-test"
```

`gl-test` draws into a 300x200 renderbuffer, 3x2 macrotiles, with EGL
surfaceless and checks the result on the CPU. Run it detached, since a
command left running when `tsh.py` disconnects is killed:

```sh
build/venv/bin/python tools/tsh.py "setsid chroot /mnt/a gl-test > /mnt/a/tmp/gl.txt 2>&1 < /dev/null &"
```

On screen, GBM on the display node (`/dev/dri/card1`, `apple-h11p`) goes
through kmsro to the GPU. The display pipe only scans contiguous memory, so
the display allocates scanout buffers as dumb buffers and the GPU imports
them, linear at the display's pitch. kmscube from Alpine draws at about 470
fps (flips do not wait for vblank):

```sh
build/venv/bin/python tools/tsh.py "chroot /mnt/a sh -c 'sleep 30 | kmscube -D /dev/dri/card1'"
```

Without the phone, build Mesa with `-Dtools=drm-shim` and run under
`LD_PRELOAD=libasahi_noop_drm_shim.so ASAHI_MESA_DEBUG=trace`.

### Graphics

Clears, triangles, points, lines, smooth and flat varyings, textures,
blending, and depth and stencil tests draw. Layouts are from AGXMetalA12
(`RenderContext<G11>::encodeAndEmitRenderState` and the state it builds).

| what | G11 |
|---|---|
| VDM vertex state | Apple's `VsStateToken`, 4 words, no presence bits: `0x48000000 \| class` (outputs <9 0x4000, <=24 0x2000, <=32 0x1800, <=64 0x1000, <=128 0x800, else 0x400), plus 0x8040 with rasterization off and the provoking vertex in 2:1, counts, `pipeline \| ceil(regs/8) << 2 \| length/64`, output count |
| draw | `0x6e040000 \| prim << 8`, then count, instances, start; indexed `0x7e020000 \| index size << 17 \| buffer hi`, then the buffer's low word, count, instances, base vertex |
| restart | before each indexed draw: `0x50000000 \| vertex state header & 0x7ffffdf \| enable << 5`, then the index (`drawIndexedPrimitives`). G13's restart state wedges now and then |
| counts | clear preshader bits 15:12, and textures 8:4 and samplers 11:9 when unused, or the USC loads state from USC 0 |
| fragment shader word | the combined word plus imageblock: bit 8 uses local memory, 23:16 its size in 256 B, 25:24 log2 samples, 31:26 tile layout (32x32 47, 32x16 63, 16x16 54), 35:32 bytes per sample / 8 |
| `st_var` | `0x01 \| last << 7 \| 1 << 8 \| reg halves << 10 \| slot << 16 \| 1 << 22 \| 0x8 << 28`, high register bits at 24 and slot bits at 26 as on G13. Immediate slots only (none of Apple's set bit 22 clear), so the geometry shader copy program that transform feedback draws through is compiled per fragment shader flat/linear inputs |
| `iter`, `iterproj`, `ldcf` | `0x11`, `0x51`, `0x91` (G13's minus 0x10; 0x21 is the texture sample): destination 15:8, cf 21:16, channels 23:22, W's cf 29:24; bit 31 adds G13's upper word (sample index, interpolation, destination bits 9:8 at 56). Immediate cf only, so Mesa lowers indexed varyings |
| `st_tile`, `ld_tile` | `0x09`, `0x79`: register 14:7, offset byte 2, coordinates byte 4 with bit 49, mask 55:52, sample mask 63:56 |
| `zs_emit` | `0x41`, G13's with the sample mask a bit higher (immediate 9, low bits 15:10) and the depth/stencil register two bits higher (23:18) |
| `sample_mask` | `0xc1`, the samples as for `zs_emit`, then the value: immediate 17, low bits 23:18. Apple's discard is `c1fe0603` |
| scissor | 8 bytes, G13's without the Z range |

PPP state goes in one block per group, in this order (the varying counts
come before the vertex state). The header bits are
not G13's: G11 reads G13's depth bias/scissor bit 8 as half of the fragment
shader.

| header | words |
|---|---|
| 0xc0000 | varying counts, 32 and 16 bit, sent with the vertex state |
| 0x180 | fragment shader: counts, `pipeline \| ceil(regs/8) << 2`, varying table, varying count |
| 0x20000 | output select, bit 16 always set, 31:24 the point size's output word plus one |
| 0x1f | fragment control, front face, front stencil, back face, back stencil |
| 0xc00 \| (n - 1) << 12 | region clips, then viewports, no viewport control word |
| 0x210000 | W clamp (1e-20), cull |
| 0x20 | scissor \| depth bias << 16 |

| word | bits |
|---|---|
| fragment control | 13:0 visibility index (offset / 4), 14 boolean, 15 visibility on, 16 scissor, 17 depth bias, 18 and 19 stencil, 21 tag write off, 26 tri merging off |
| face | 7:0 stencil reference, 15:8 line width, 19:17 depth compare, 20 shader writes depth (without it the test uses the interpolated depth; the written depth is not clamped, so Mesa saturates it), 21 depth write off, 26:24 pass type, 31:28 object type (5 and 6 line and point filled triangles) |
| stencil | as G13 |
| cull | as G13, clamp W in bit 5, no cull 2 |
| shader counts | as G13 up to bit 11; from 15 the tilebuffer words the shader writes, 0 while every render target is spilled |

The block store (`imgwblk`) encoding is unknown and `ld_tile` hangs in the
end of tile program, so Mesa spills every render target: the background and
fragment programs write the image directly (PBE at state register 2 rt + 1,
32-bit data) and the end of tile program does nothing. Render targets are
bound as 2D at their first layer: the array write ends the thread, which
dropped every render target after the first.
For the same reason Mesa's compute blitter, which stores through `imgwblk`,
is off: blits and `glGenerateMipmap` draw. The blend epilog reads the render
target through Mesa's txf sampler, so the fragment pipeline loads samplers
whenever render targets spill, even when the shader has none of its own.
The texture unit does not see what the clear or earlier draws wrote in the
same pass, so a draw that blends, masks or reads the framebuffer after them
starts a new pass. That is slow, and overlap within one draw is still wrong.

A12 has no BC (S3TC, RGTC, BPTC) decoding; sampling S3TC hangs it. Mesa
decompresses those formats on upload instead. It has no ASTC HDR either: HDR
blocks read as zero through the HDR formats and as the LDR error colour
through the LDR ones, so Mesa leaves out KHR_texture_compression_astc_hdr.

Depth and stencil need little new: G11's ZLS control word and the depth and
stencil load and store registers are G13's (AGXMetalA12 `Framebuffer<G11>`
and the kext's copy into the 3D command), and the depth compare in the face
word is Metal's compare function, the same order as G13. AGXMetalA12 passes
a Z16 clear depth as `(float)(int)(depth * 65535)`, but the register wants
the integer: the float clears to the wrong depth. With a stencil attachment
Apple also sets ZLS bits 62 and 63; without them loading stencil hangs the
3D pass.

Mip levels need texture word 0 bit 63, which AGXMetalA12 sets when the
texture has more than one level (`TextureGen3<G11>` keeps Metal's
`mipmapLevelCount` at 0x84). Without it every level samples as level 0.
Apple sets PBE bit 51 the same way, but with it Mesa's image writes lose
level 0, so Mesa leaves it clear. The image write takes level 0 from the
instruction, so to render into a level Mesa points the PBE at that level,
with its size and one level.

Multisampled textures: AGXMetalA12 gives them dimension 4 and the sample
count as on G13, but how the sample instruction picks a sample is not known
(with G13's coordinates every read returns the first pixel), and the image
write has no sample either. Mesa keeps the hardware out of it: a
multisampled colour buffer is a linear 2D array with a layer per sample,
read as an array and written as one tall 2D image at y + sample * height,
with the fragment shader looping over samples. Depth and stencil stay in
ZLS's layout, a pixel's samples side by side, and are read as one 2D layer
of a format with a channel per sample (Z32F 4x as RGBA32F), so blits can
pick the sample. Rasterization, coverage and ZLS need nothing new. Centroid
never shows up in Apple's code; Mesa uses G13's value.

The kernel side needs the hwdata power limits (0 parks the engines), the
scene at 3D command +0x30, the event counter written at both finalizes, the
utiles per macrotile in 3D register 0x16038 (without it only the first
macrotile renders), and the kext's tiling sizes: region headers
`5 * macrotiles * utiles / 4` dwords, the region array and TPC in 4K pages
at 0x50 and 0x80 bytes per macrotile utile, and a buffer manager whose block
list entries are eight 16K chunks. The parameter heap behind it is 8 MB of
its own; 1 MB ran out at 3000 full screen triangles. The tiling buffers are
sized for 16384x4096 worth of macrotiles, in any shape up to 16384 a side. There are no partial
renders yet, so a frame that fills the heap hangs the TA. The scene's
`+0x14` counts the 128K blocks a TA used: 14 B a point, 32 B a tiny
triangle, about 0.6 B per 32x32 tile a primitive covers. Mesa keeps an
estimate per render (4 B per vertex output plus 8, `agx_g11_heap_cost`),
ends the render past 6 MB and splits draws bigger than half that.

### Vulkan

Honeykrisp (`src/asahi/vulkan`) runs vkcube under weston. Its G11 paths are
the GL driver's: the vertex state token, PPP state in G11's blocks, 8 byte
scissors, the combined shader word, spilled render targets, linear images
without compression, and the G11 PBE stride bias and level handling.

| what | G11 |
|---|---|
| descriptors | no bindless: a shader's textures and samplers (constant indices) get state slots at compile time, and every draw and dispatch copies their descriptors from the bound sets into its texture and sampler state, spilled render targets after the textures. Samplers 1-14 are the API's, 0 the txf sampler and 15 the txf-lod one. Vulkan forbids updating a bound set, so copying at record time is safe; there is no update-after-bind |
| CDM stream link | not known, so compute streams are not merged; a single stream past 64 KiB (about 20 dispatches) would still need it. VDM's link works |
| copies | the meta copy shaders store texel by texel; G11's block store is not known |
| WSI | linear modifiers only. weston logs `bo geometry out of bounds` while it tries the buffer on a display plane, then composites it with GL |
| internal kernels | libagx's precompiled kernels (buffer copies, fills, updates) launch with no texture or sampler states; a count without the USC words loads state from address 0 and left every later compute job doing nothing |
| layers | layered rendering is not known and the kernel takes one layer: layer writes are dropped and everything lands on layer 0 |
| background loads | background programs clamp spilled render target loads to the image: they run for the whole tile, past the last row of a linear image. Fragment shaders reading the framebuffer are in bounds and skip it |
| faults | any GPU fault wedges G11 until reboot (no soft faults). So every buffer access is bounds checked whatever the pipeline asks for, and texture states that are unwritten or past the end of their set get the null texture |
| dynamic indexing | a descriptor array indexed at run time becomes a chain of accesses with constant indices, at most 16. Samples and loads reach states 0-15 only: past that they read zero, stores go up to 48 |
| spilled targets | fragment shaders read a spilled render target's size from its descriptor through u0_u1, which points at the draw's texture states |
| draws | an indexed draw follows a restart token, as in GL. libagx kernels that write draws into the stream fill exactly the G11 size: a gap is read as more words |
| LOD query | not in hardware: computed from coordinate derivatives, without the sampler's bias and clamps. A null texture queries zero |
| blending | blends read the spilled render target through the texture unit, which does not see what earlier draws in the render wrote. A draw that reads its render targets after other draws starts a new render, as in GL |
| layered writes | an image write to an array, 3D or cube image ends the thread. Such storage views get a 2D buffer-like PBE and the shader writes at the texel's index (`lower_g11_layered_store`), from the tile size, aligned width and layer stride in the PBE's software words; image atomics use the same address |
| twiddling | Y takes the low bit, and the long side's extra bits of an oblong tile sit on top (8 byte texels have 32x64 tiles). Shader-side addressing follows ail's CPU tiling |
| fetch out of bounds | a fetch above level 0 is a txl through the txf-lod sampler, which clamps the level and layer: an out of range level or layer moves the texel out of bounds to read the zero border |
| barriers | `memory_barrier_2` and the texture flush end the thread, so the fence after an image atomic is only `memory_barrier` and the image barriers |
| blits | one layer per render, through 2D views of the layer or 3D slice (`blit_per_layer` in vk_meta) |
| border colours | the sampler has no border colour: clamp to border reads transparent black whatever was asked (Metal has no border colour on A12 either) |
| R9G9B9E5 copies | copied as R32_UINT, G11 writes the shared exponent format wrong |

dEQP-VK, every 200th case of the mustpass (16112): 3848 pass, 98 fail, 10901
not supported, 40 internal errors (the test cannot handle the format) and 2
that time out, and none wedges the GPU. The fails are opaque border colours,
shaders reading more than 16 textures, 3D texel fetches, layered clears and
multiview. Vulkan 1.3 is reported: 1.4 needs subgroup operations G11 does not
have (only barriers and elect work), and the GPU caps at 740 MHz unless told
otherwise (see Power).

Not done: layered rendering and multiview, multisampling (only 1 sample is
advertised, so this is not conformant), geometry and tessellation shaders and
transform feedback (they hang in the TA), dual source blending, sparse
residency, more than one viewport, custom and opaque border colours, more than
16 texture reads per shader. A bad job still wedges the GPU until reboot.

```sh
build/venv/bin/python tools/tsh.py "chroot /mnt/a vulkaninfo --summary"
```

## DRM

`linux-0020` is `drivers/gpu/drm/agx`, a render node with the
Asahi uAPI (`include/uapi/drm/asahi_drm.h`).

| | |
|---|---|
| VMs | one MMU context each, 16 to 63 |
| VA | 16K to 0x80_0000_0000, 16K pages |
| `usc_exec_base` | must be 0x11 << 32, the base the firmware loads |
| submit | async, one `drm_sched` job per compute or render command |
| name | `asahi`, so Mesa picks it up |
| imports | dma-bufs map their pages for the GPU (`drm_gem_shmem_prime_import_sg_table`), for display scanout |
| binds | single-page binds map one page over the range; a bind over existing mappings replaces them, splitting those it cuts (Vulkan rebinds sparse ranges) |
| timestamps | reported in ns, as Honeykrisp expects; not written |
| ignored | sampler heaps |
| not yet | helper programs, more than one render in flight |

Every user context gets a page catalogue at init and is loaded by the
firmware at every power-up, so a VM only fills its page tables. After a
change the driver sets the context's TLB generation to the firmware's copy
plus 1 << 22, and the firmware flushes before that context's next job.

The kernel range each VM reserves holds a zeroed page for RGX 0x14020.

Each queue is an entity on one scheduler that runs one job at a time, so a
submit's commands run in order. In-syncs gate the first command and
out-syncs get the last one's fence. When a job ends the firmware writes its
stamp and sends 0x42 on endpoint 0x21, and the driver then signals the
job's fence. The firmware cannot be reset, so a job that times out wedges
the GPU until the next boot. Unbinds wait until the VM has no jobs queued
or running.

After a job times out, the driver prints the MMU fault status (RGX 0x17040).

## Queues

Several commands can go through one queue in one firmware session. The run
message is `{type, info, head, queue index, flush}`. The firmware stores head
at info+0x28 and runs the ring from its read index up to it. The next command
goes in the next ring entry with head + 1.
