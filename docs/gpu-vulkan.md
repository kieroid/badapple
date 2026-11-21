# GPU Vulkan and tests

## Honeykrisp

`src/asahi/vulkan` runs vkcube under weston, with the GL driver's G11 paths.

| what | G11 |
|---|---|
| descriptors | no bindless: a shader's textures and samplers get state slots at compile time, and each draw copies them from the bound sets. Samplers 1-14 are the API's, 0 txf, 15 txf-lod. No update-after-bind |
| CDM stream link | not known, so compute streams are not merged; one stream past 64 KiB (about 20 dispatches) would need it |
| copies | meta copy shaders store texel by texel |
| WSI | linear modifiers only. weston's `bo geometry out of bounds` is harmless |
| internal kernels | libagx kernels launch with no texture or sampler states; a count without the USC words broke every later compute job |
| layers | not known; one render per layer (up to 256). The vertex shader moves other layers' primitives out of the clip volume, writing the position once at the end |
| background loads | clamped to the image, since background programs run for the whole tile |
| faults | any fault wedges G11 until reboot, so every buffer access is bounds checked and missing texture states get the null texture |
| dynamic indexing | a chain of constant index accesses, at most 16. Samples reach states 0-15, stores up to 48 |
| spilled targets | the shader reads the target's size from its descriptor through u0_u1 |
| draws | indexed draws follow a restart token. Kernels that write draws fill exactly the G11 size |
| LOD query | computed from derivatives, without the sampler's bias and clamps |
| blending | as in GL: tilebuffer copy, else the texture unit and a new render |
| layered writes | array, 3D or cube writes end the thread, so storage views get a 2D buffer-like PBE and the shader writes at the texel index (`lower_g11_layered_store`) |
| twiddling | Y takes the low bit, long side's extra bits on top. Shaders follow ail's CPU tiling |
| fetch above level 0 | a txl through txf-lod, which clamps level and layer; out of range reads the zero border |
| barriers | `memory_barrier_2` and texture flush end the thread, so only `memory_barrier` and image barriers |
| blits | one layer per render (`blit_per_layer`) |
| border colours | none in hardware: clamp to border reads transparent black. For opaque borders the shader adds the colour times the weight of texels outside the image (`g11_lower_border`). Skipped past 32 samples; no anisotropy or depth compare |
| preambles | none for shaders with texture instructions |
| texture registers | samples, image writes and tile accesses reach r0-r31 only; the allocator moves operands there. A sample may not take the registers of a source it kills (early kill) when they sit above r31: a `textureGrad` result went to its high gradient registers and was lost |
| clip distances | output select 31:24 must be set even with only clip distances. Only the first plane works |
| R9G9B9E5 copies | as R32_UINT |
| multisampling | 2x and 4x, as in GL: colour is a linear array of a layer per sample, depth and stencil keep ZLS's layout. The descriptor carries log2 samples in bits 191:190 (software), and a sample fetch reads layer * samples + sample. Colour copies are byte row kernels; depth and stencil copy as one wide texel per pixel. No sample shading, no multisampled storage images |
| texture slots | samples reach states 0-15 only: the 4-bit field has no high bits (every free sample bit tried with 32 states bound, none reached state 16). Reads get slots first, stores the ones after |
| compressed views | an uncompressed view of one compressed mip level leaves the mipmapped bit clear and describes one level, or the hardware reads another layout |
| LOD query | computed from derivatives (G11 has none). hk then clamps the level to the sampler's minLod and maxLod and rounds it for the nearest mip filter, read from the sampler word. All 190 `texturequerylod` cases pass. GL's query skips the clamps |
| image store then load | a load of a texel the same invocation just stored can read the old value when neighbouring texels belong to other workgroups: stale texture cache lines. Mesa's `image_barrier` fences, `0xf596`, waits and the G13 load cache bits (43 to 47) do not help, and G11 has no `flush_memory_to_texture`. The 13 ASTC `texel_view_compatible` `image_store` fails are this |
| sparse | binding only. Resident and aliased sparse images are refused in the format query: dEQP made them anyway when the format query allowed it, and their reads wedged the GPU. The kernel takes 256 bind ops a call, so Mesa splits bigger binds |
| gathers | none in hardware. Each gather is four samples at texel centres, so the sampler still wraps, borders and compares; linear filtering at a centre returns the texel. Cube texels off the face go to the next face's edge texel. Every 5th `texture_gather` graphics case: 146 pass, no fails |
| min LOD | no LOD modes with a minimum. `textureClamp` samples at max(LOD, min) with the LOD from derivatives, gradients go through `nir_lower_tex`. Every 3rd `*clamp*` case: 756 pass, 2 fail (`mirrored`) |
| LOD 0 | a `txl` with constant LOD 0 is not encoded as automatic LOD in fragment shaders: G11 takes that from derivatives. Texel fetches keep it, they broke otherwise |
| coherent memory | plain loads and stores stay in the core's cache: no barrier makes them visible to another core, no load or store cache bit helps, and the big barriers G13 uses end the thread. Atomics are coherent, so coherent 32 and 64-bit buffer accesses (out of bounds loads skip the atomic, which would write the read-only zero page) and coherent R32 integer image accesses become atomics (`g11_coherent_atomics`, `g11_coherent_image_atomic`). Other image formats stay plain. SPIR-V texel availability (`devicecoherent`) leaves its barrier before the store and after the load, where `nir_lower_memory_model` drops it, so the G11 pass runs first and reads those barriers. Every 10th `message_passing` case: 81 pass, no fails (35 failed before) |
| varying index | `st_vary` with a register index clears bit 22, for separately compiled vertex shaders |

dEQP-VK, every 200th mustpass case (15441): 4199 pass, 16 fail, 11181 not
supported, 40 internal errors (ASTC and EAC blits), 5 resource errors, no
hangs or GPU wedges. Fails: 12 `descriptorset_random` (more than 16
textures), 2 `mutable_descriptor`, 1 clip distance (only the first plane
works), 1 multiview query. The earlier run's fp16 denormal, memory model and
null descriptor fails and its one hang are gone. A resource error (a missing
amber file) aborts the dEQP process, so the cases after one were rerun alone.
Every 20th multisampling case (2795): 311 pass, no fails. Vulkan 1.3; 1.4
needs subgroups.

Texture sweep (every 16th `texture` case, every 5th `texture_functions` and
`texture_gather` case, robustness2 sampled images): no change from the early
kill fix, and GLES3 texture functions pass 1678 of 1679. Older bugs it found:

- `texture_functions` cases with the `mirrored` wrap fail. That is
  mirror-clamp-to-edge, which G11 does not have and hk does not advertise
  (`samplerMirrorClampToEdge`), but dEQP uses it without checking. The wrap
  field acts as 2 bits on G11: 4 to 7 repeat 0 to 3.
- Every `texture_gather` case and the `*clamp*` cases aborted in the
  instruction packer (no gather, no min LOD modes on G11). The case runner
  logged the abort as a hang, so they looked like slow compiles. Fixed, see
  the table.

All 9872 `texture.swizzle` cases: 2056 pass, the rest not supported. Every
10th `sparse_resources` case (1940): 186 pass, no fails.

GLES dEQP needs `EGL_PLATFORM=surfaceless`: Mesa is built with X11 too, so
EGL tries X11 first and fails without a display.

Missing: sample shading, 8x and 16x samples, geometry,
tessellation and transform feedback (they hang the TA), dual source blending,
sparse residency, multiple viewports, custom border colours, more than 16
texture reads per shader.

## Running

`make rootfs` cross-builds Mesa, `tools/cl-test.c` and `tools/gl-test.c` into
an Alpine rootfs. `tools/rootfs-push.sh` copies it to a tmpfs on the phone.

```sh
make rootfs
./tools/rootfs-push.sh
build/venv/bin/python tools/tsh.py "chroot /mnt/a env RUSTICL_ENABLE=asahi cl-test"
```

`gl-test` draws a 300x200 renderbuffer with EGL surfaceless and checks it on
the CPU. Run it detached, since `tsh.py` kills what it leaves running:

```sh
build/venv/bin/python tools/tsh.py "setsid chroot /mnt/a gl-test > /mnt/a/tmp/gl.txt 2>&1 < /dev/null &"
```

On screen, GBM on `/dev/dri/card1` (`apple-h11p`) goes through kmsro to the
GPU. The display only scans contiguous memory, so scanout buffers are dumb
buffers the GPU imports, linear. kmscube runs at about 470 fps:

```sh
build/venv/bin/python tools/tsh.py "chroot /mnt/a sh -c 'sleep 30 | kmscube -D /dev/dri/card1'"
build/venv/bin/python tools/tsh.py "chroot /mnt/a vulkaninfo --summary"
```

Without the phone: build Mesa with `-Dtools=drm-shim` and run with
`LD_PRELOAD=libasahi_noop_drm_shim.so ASAHI_MESA_DEBUG=trace`.
