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
| varying index | `st_vary` with a register index clears bit 22, for separately compiled vertex shaders |

dEQP-VK, every 200th mustpass case (15486): 4220 pass, 21 fail, 11220 not
supported, 40 internal errors, no GPU wedges.
`textureoffsetclamp.clamp_to_border.sampler3d_bias_float_fragment` times out
and is skipped. Fails: more than 16 textures, ASTC stores through views of
mip levels past 0, fp16 denormals, a racy memory model case. Every 20th
multisampling case (2795): 311 pass, no fails. Vulkan 1.3; 1.4 needs
subgroups.

Texture sweep (every 16th `texture` case, every 5th `texture_functions` and
`texture_gather` case, robustness2 sampled images): no change from the early
kill fix, and GLES3 texture functions pass 1678 of 1679. Older bugs it found:

- `texture.swizzle` with packed formats (a4r4g4b4, a2r10g10b10) fails and
  wedges the GPU (MMU read fault, requester 16).
- `texture_functions` offsets with the `mirrored` wrap, and
  `texturequerylod` `clamp_min` and `clamp_max`, fail.
- Some `clamp` and `texture_gather` cases take over 5 minutes to compile.

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
