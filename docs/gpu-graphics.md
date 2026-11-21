# GPU graphics

Clears, triangles, points, lines, varyings, textures, blending, depth and
stencil work. Layouts are from AGXMetalA12
(`RenderContext<G11>::encodeAndEmitRenderState`).

## State

| what | G11 |
|---|---|
| VDM vertex state | `VsStateToken`, 4 words, no presence bits: `0x48000000 \| class` (outputs <9 0x4000, <=24 0x2000, <=32 0x1800, <=64 0x1000, <=128 0x800, else 0x400), 0x8040 with rasterization off, provoking vertex 2:1; counts; `pipeline \| ceil(regs/8) << 2 \| length/64`; output count |
| draw | `0x6e040000 \| prim << 8`, count, instances, start. Indexed: `0x7e020000 \| index size << 17 \| buffer hi`, buffer low, count, instances, base vertex |
| restart | before each indexed draw: `0x50000000 \| header & 0x7ffffdf \| enable << 5`, then the index. G13's restart state wedges now and then |
| counts | clear preshader 15:12, textures 8:4 and samplers 11:9 when unused, or the USC loads state from address 0 |
| fragment shader word | the combined word plus imageblock: bit 8 local memory, 23:16 its size in 256 B, 25:24 log2 samples, 31:26 tile layout (32x32 47, 32x16 63, 16x16 54), 35:32 bytes per sample / 8 |
| `st_var` | `0x01 \| last << 7 \| 1 << 8 \| reg halves << 10 \| slot << 16 \| 1 << 22 \| 0x8 << 28`, high bits as G13. Immediate slots only |
| `iter`, `iterproj`, `ldcf` | `0x11`, `0x51`, `0x91`: destination 15:8, cf 21:16, channels 23:22, W cf 29:24; bit 31 adds G13's upper word. Immediate cf only, so indexed varyings are lowered |
| `st_tile`, `ld_tile` | `0x09`, `0x79`: format 9:7 (I16 1, I32 2, U8NORM 4), register halves 15:10 (r0-r31), byte offset << 4 in byte 2, coordinates byte 4 with bit 49, load 51, mask 55:52, sample mask 63:56. 32-bit registers for I32, else 16-bit |
| `zs_emit` | `0x41`, as G13 with the sample mask one bit higher (immediate 9, low bits 15:10) and the register two bits higher (23:18) |
| `sample_mask` | `0xc1`, samples as `zs_emit`, value immediate 17, low bits 23:18. Apple's discard is `c1fe0603` |
| scissor | 8 bytes, G13's without the Z range |

PPP state goes in one block per group, in this order. The header bits are not
G13's: G11 reads G13's bit 8 as half of the fragment shader.

| header | words |
|---|---|
| 0xc0000 | varying counts, 32 and 16 bit, sent with the vertex state |
| 0x180 | fragment shader: counts, `pipeline \| ceil(regs/8) << 2`, varying table, varying count |
| 0x20000 | output select, bit 16 set, 31:24 the point size's word plus one |
| 0x1f | fragment control, front face, front stencil, back face, back stencil |
| 0xc00 \| (n - 1) << 12 | region clips, viewports, no viewport control word |
| 0x210000 | W clamp (1e-20), cull |
| 0x20 | scissor \| depth bias << 16 |

| word | bits |
|---|---|
| fragment control | 13:0 visibility index (offset / 4), 14 boolean, 15 visibility on, 16 scissor, 17 depth bias, 18, 19 stencil, 21 tag write off, 26 tri merging off |
| face | 7:0 stencil reference, 15:8 line width, 19:17 depth compare, 20 shader writes depth (not clamped, so Mesa saturates), 21 depth write off, 26:24 pass type, 31:28 object type |
| stencil | as G13 |
| cull | as G13, W clamp bit 5, no cull 2 |
| shader counts | as G13 to bit 11; from 15 the tilebuffer words written |

## Render targets

The block store (`imgwblk`) is not known, so the end of tile program does
nothing and render targets live in memory. Background and fragment programs
write the image directly (PBE at state register 2 rt + 1). Targets bind as 2D
at their first layer, since an array write ends the thread. Mesa's compute
blitter uses `imgwblk`, so blits and mipmap generation draw instead.

Single-sampled U8NORM, I16 and F16 targets also get a tilebuffer copy, up to
16 bytes a pixel, and blends read it with `ld_tile` as Apple's do. The copy
costs a tile store per fragment (10% on glmark2's plain scenes), so a batch
starts without it and turns it on, in a new batch, at the first draw that
reads its targets. A batch with only fast clears turns it on in place: GTK
clears then blends, and Nautilus scrolling went from about 5 extra renders a
frame to under 1. Vulkan does the same with a new control stream. Layered
renders and draw clears keep the copy from the start. The background program
reloads the copy and never writes memory back (that raced the draws). F16
goes through the tile as I16.

Other targets blend through the txf sampler, which does not see earlier
writes in the same pass, so a draw that reads them starts a new pass. Slow,
and overlap within one draw is still wrong.

Samples and image writes reach texture and sampler states 0 to 15. Render
targets are always spilled, so their texture and PBE descriptors follow the
shader's textures. GLES3's 16 textures plus a render target need 18: such a
draw is dropped with a warning, since a wrapped state index read or wrote
through another descriptor and wedged the GPU. A shader using API sampler 15
gets samplers 0 to 15 for API samplers 0 to 15 and no txf sampler, so vertex
shaders and draws without colour targets can use all 16.

## Depth, stencil, mipmaps, MSAA

- ZLS control and depth/stencil registers are G13's. Z16 clear depth wants
  the integer, not AGXMetalA12's float. With stencil, ZLS bits 62 and 63 must
  be set or stencil loads hang the 3D pass.
- Mip levels need texture word 0 bit 63, or every level samples as level 0.
  PBE bit 51 loses level 0 on writes, so it stays clear and the PBE points at
  the level being rendered.
- Multisampled textures: how a sample is picked is not known. A multisampled
  colour buffer is a linear 2D array, a layer per sample, written as one tall
  image at y + sample * height, and the fragment shader loops over samples.
  Fetches read layer * samples + sample, samples from descriptor bits
  191:190 (software, past the linear layer stride).
  Depth and stencil keep ZLS's layout and are read as a format with a channel
  per sample.

## Tiling heap

The kernel needs the hwdata power limits, the scene at 3D command +0x30, the
event counter at both finalizes, and utiles per macrotile in 3D register
0x16038 (or only the first macrotile renders). Sizes from the kext: region
headers `5 * macrotiles * utiles / 4` dwords, region array and TPC in 4K
pages at 0x50 and 0x80 bytes per macrotile utile, block list entries of eight
16K chunks. The parameter heap is 8 MB.

Tiling buffers cover 8192x4096 worth of macrotiles. Twice that wedges the
firmware, so the kernel rejects it and Mesa caps textures at 8192 and render
targets at 8192x4096. No partial renders yet, so a full heap wedges the
firmware silently: jobs complete and write zeros.

The scene's `+0x14` counts the 128K blocks a TA used: 14 B a point, 32 B a
tiny triangle, about 0.6 B per 32x32 tile covered, plus 20-27 B per tile each
draw touches. Mesa estimates per render (4 B per vertex output plus 8, plus
24 B per tile in the viewport and scissor, `agx_g11_heap_cost`), ends the
render past 6 MB and splits draws bigger than half that. Honeykrisp does the
same (`hk_g11_heap_reserve`).
