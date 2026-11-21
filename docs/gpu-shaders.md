# GPU shaders

## Compute

CDM block type in bits 31:30: 0 launch, 2 terminate (0x80000000), 3 barrier
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

The state block is 64 byte aligned. Low bits of its address: 5:2 registers in
blocks of 8 (0 is 16), 1:0 length in 64 bytes. Contents:

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

Uniform addresses are full GPU VAs. Code addresses are 32 bit offsets from
0x11 << 32. The driver adds 16 bytes to the threadgroup memory size.

## ISA

Close to G13. G11 kernels (Mach-O, cpusubtype 0x72) are in the iOS 16
CoreImage archives, and for iOS 18 also in QuartzCore, ImageIO and RenderBox.
applegpu's G13 disassembler reads nearly all of them.

| | G11 |
|---|---|
| stop | `0x0801` |
| threadgroup load, store | device load, store (`0x05`, `0x45`) with bits 29:28 = 2, bit 26 and 46:45 clear, a 16-bit register base with the byte address |
| threadgroup barrier | `0x0068`, as G13 |
| memory barrier | only `0xf596` |
| flush memory to texture | none, the thread stops there |
| SIMD reductions | none |

A missing instruction kills the thread without a fault, and the job still
completes. A store's register offset is scaled by the element size.

## Mesa state

`patches/mesa-0001-asahi-g11.patch` points Mesa's Asahi compiler and Gallium
driver at G11: OpenCL through Rusticl, GLES through EGL. `AGX_G11` is in
`genxml/agx_g11.h`.

| state word | G11 |
|---|---|
| uniform | tag 0x03 |
| shader | one word for shader, registers and threadgroup memory |
| no preshader | 0x0108 |
| preshader | 0x38, 0xc04000, then the code address. Fragment preshaders go stale across draws, and one that samples drops triangles, so fragment shaders and shaders with textures get none |
| texture | `0x17 \| start << 8 \| count << 20 \| address >> 2 << 26`, count 0 is 64 |
| sampler | `0x13 \| start << 8 \| count << 20 \| address >> 2 << 26` |
| CDM header | bit 0 set; uniforms, textures in 8s, samplers in 4s |

| what | G11 |
|---|---|
| sqrt | no srsqrt: x * rsqrt(x), 0 and inf passed through |
| rounding | floor only; ceil, trunc and roundeven built from it |
| SIMD groups | none, so no subgroups |
| half denormals | flushed in f16 arithmetic sources, compares and constants, but min, max and clamp pass them through. Vulkan gets neither denorm preserve nor flush-to-zero for f16 |
| threadgroup atomics | the device atomic with address space 2 in bits 29:28 |
| thread index in threadgroup | built from position and size |
| stack | G13's stack instructions kill the thread |
| uniforms | 256 halves; u256 and up not known, so no further promotion |

Private memory is a global buffer per launch, indexed from the grid position.
Graphics stages have none, so their arrays stay in registers. Spills go to the
top of a full 32 KB threadgroup window, one slice per thread from r3l, which
caps the threadgroup size.

## Images

From AGXMetalA12, and Apple's G11 kernels lined up against the same kernels
for G13 (iPhone13,2 IPSWs, iOS 16.6.1 and 18.6.2).

| what | G11 |
|---|---|
| texture, PBE descriptors | as G13, but word 0 from bit 3 up is one bit lower. Texture bit 63 (PBE 51) set with more than one level, samples at 65:64 (PBE 57:56). Texture word 1 bit 38: a companion buffer follows in word 2 |
| sampler | as G13, 8 bytes |
| sample | 10 bytes, `0x21`, `0xa1` for 3D and arrays: destination halves 15:10, texture 19:16, sampler 23:20, 32-bit destination 24, coordinates 29:25, mask 35:32, LOD mode 42:40, LOD 55:48, dimension 9:8, bit 77 set. The tail's low byte is the offset/compare register. Registers above r31 read garbage, so Mesa keeps sampling shaders in r0-r31. 4-bit sampler index: 16 samplers, Mesa's txf at 0 and txf-lod at 15 |
| image write | 8 bytes, `0x31`: data halves 15:10, coordinates 19:16 and 39:36, state 23:20 and 35:32, dimension 9:8. Arrays: `0xb1`, bit 63, a 2-byte tail with `0x30` in 69:68. Without the tail an array write is dropped; with it the write lands and the thread stops |
| offsets, compare | G13's packed offsets or depth reference in a register at 71:64; bit 78 for offsets, 79 for compare |
| texture_load | none; Mesa samples the pixel centre. texelFetch above level 0 uses normalized coordinates and an explicit LOD through the txf-lod sampler |
| bindless textures | none |
| twiddled tiles | G13's sizes, but upright for 2, 8 and 32 byte formats: 64x128, 32x64, 16x32. Z order from Y, extra long side bits on top |
| layout | images are linear, 2D arrays too |

CTS kernel_image_methods, samplerless_reads, cl_read_write_images and
cl_fill_images pass. Still wrong:

- twiddled writes of 8-byte formats from 128x64 up (unused, images are linear)
- linear filtering: only 16-bit unorm is in tolerance. 8-bit, snorm, half and
  sRGB use about 1/32 weights, float32 does not filter, so neither driver
  offers float32 linear filtering. The sampler has no precision bit.

Metal's own samplers load indirectly, `0x07 | start << 8 | count << 20 |
index << 27`, index not understood. A bad sampler load breaks texturing until
reboot.

A12 has no BC decoding (sampling S3TC hangs it), so Mesa decompresses on
upload. ASTC HDR reads as zero or the error colour, so it is not offered.
