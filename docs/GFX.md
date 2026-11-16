# any360::gfx: the native graphics layer

`any360::gfx` is the GPU half of stage 1 ([DESIGN.md](DESIGN.md)): what replaces ReXGlue's emulated Xenos. It is
portable C++23 with no dependency on a graphics API. The renderer behind it is Vulkan, so it runs on Windows, Linux
(Steam Deck first) and Android, and on macOS through MoltenVK. The front end below compiles on all of them unchanged.

## Layout

| Path | What it is |
|---|---|
| `include/any360/gfx/registers.h` | The register indices the command processor acts on, the type-3 opcodes, packet builders |
| `include/any360/gfx/command_processor.h`, `src/command_processor.cpp` | `CommandProcessor` (a `sys::GpuBackend`), `Renderer`, `DrawCall`, `Shader` |
| `tests/gfx_cp_tests.cpp` | Scenario tests: packets written by hand, executed, effects read back |
| `third_party/xenos_shader/` | The Xenos microcode to SPIR-V translator, vendored from the ReXGlue SDK (Xenia's, BSD-3): upstream files at their own paths, two marked edits, `shim/` for the SDK infrastructure it included (`NOTICE.md` lists every change) |
| `third_party/glslang/`, `third_party/fmt/`, `third_party/vulkan-headers/` | The SPIR-V builder the translator writes with, header-only fmt, the Khronos headers (each with its license and notice) |
| `include/any360/gfx/vulkan/device.h`, `src/vulkan/device.cpp` | The Vulkan device: the loader opened at run time (no SDK, no import library; Windows, Linux, macOS through MoltenVK), Vulkan 1.3 with dynamic rendering and synchronization2, headless, buffers and images for tests and readbacks |
| `tests/gfx_shader_tests.cpp` | Microcode encoded by hand (no game data), translated as the reference pipeline cache asks, SPIR-V checked and written out |
| `tests/gfx_vulkan_tests.cpp` | On a real driver: the translated shaders compiled into pipelines with the translator's descriptor layout, an offscreen target rendered and read back (skipped without a Vulkan 1.3 device) |
| `include/any360/gfx/vulkan/renderer.h`, `src/vulkan/renderer.cpp` | `VulkanRenderer`, the command processor's `Renderer`: shaders translated and cached by hash, pipelines by shader pair and topology, the five constant buffers from the register file, guest memory mirrored for vertex fetch, DMA indices endian-swapped |
| `include/any360/gfx/vulkan/textures.h`, `src/vulkan/textures.cpp` | The texture cache: fetch constants parsed, guest memory endian-swapped and untiled (Xenos 32x32-block tiling, as the reference computes it), uploaded when it changed, viewed with the guest swizzle; samplers from the fetch constant and the fetch instruction |
| `include/any360/gfx/vulkan/presenter.h`, `src/vulkan/presenter.cpp` | Presentation: a swapchain on the window surface the platform layer makes, each swapped frame uploaded and blitted with its aspect ratio kept (FIFO) |
| `tests/gfx_window_demo.cpp` | A window (SDL3) showing the whole chain every frame: hand-written PM4, a textured quad and a moving triangle, resolve, swap, present (`gfx_window_demo [--frames N] [--gpu NAME]`, built when the reference SDK has SDL3) |
| `tests/ucode_encode.h` | Hand encoding of control flow, vector ALU and vertex fetch instructions, shared by the tests |
| `tests/gfx_render_tests.cpp` | End to end: hand-written PM4 (state, fetch constant, shaders, constants, an auto-indexed and a DMA-indexed draw) through the command processor into the renderer; every pixel read back and checked |
| `include/any360/gfx/capture.h`, `src/capture.cpp` | Frame captures ([CAPTURE.md](CAPTURE.md)): the writer a GPU front end feeds at each kick and swap, the reader, the replay through a `CommandProcessor` |
| `tests/gfx_replay.cpp` | `gfx_replay CAPTURE [--gpu NAME] [--out DIR]`: a capture replayed, the draws not drawn by reason, the PM4 packets used, each frame compared with the reference's |

`layer/sys/CMakeLists.txt` adds this directory, and `layer/sys/test.ps1` runs the gfx tests with the other tests.
The translated shaders run on the NVIDIA (RTX 5070 Ti) and AMD (Radeon integrated) Windows drivers with identical
pixels; RADV on the Steam Deck is still to run.

## The command processor

The title's D3D runtime writes PM4 into a ring buffer in physical memory and moves the write pointer through the GPU's
MMIO window (`CP_RB_WPTR`, 0x7FC80714). `any360::sys` hands the ring (`VdInitializeRingBuffer`), the read-pointer
write-back (`VdEnableRingBufferRPtrWriteBack`) and the MMIO accesses (`GraphicsKernel::WriteRegister` /
`ReadRegister`, with `IsGpuMmio` and `GpuRegisterIndex` for the binding) to its `GpuBackend`. `CommandProcessor` is
that backend. It executes the ring from the read pointer to the write pointer, on the caller's thread with `Execute`
or on its own worker with `Start`, then writes the read pointer back.

| Packets | What happens |
|---|---|
| type 0, type 1, `SET_CONSTANT`, `SET_CONSTANT2`, `SET_SHADER_CONSTANTS`, `LOAD_ALU_CONSTANT` | the register file (0x5003 dwords): context registers, ALU / fetch / bool / loop constants |
| `INDIRECT_BUFFER`, `INDIRECT_BUFFER_PFD` | the command buffer segments, nested up to 8 deep |
| `WAIT_REG_MEM`, `COND_WRITE` | a register or memory compared (never, <, <=, ==, !=, >=, >, always) under a mask; the wait holds the stream until the CPU writes, and `Stop` releases it |
| `MEM_WRITE`, `REG_TO_MEM`, `REG_RMW`, scratch registers | memory and register writes the CPU polls |
| `EVENT_WRITE`, `EVENT_WRITE_SHD` (fences, with the vertical blank counter), `EVENT_WRITE_EXT` (screen extents), `EVENT_WRITE_ZPD` (occlusion queries) | the events D3D synchronises on |
| `COHER_STATUS_HOST` | coherency: a wait on it completes the request and tells the renderer which memory is stale |
| `INTERRUPT` | the title's graphics interrupt callback, once per CPU in the mask (source 1) |
| `IM_LOAD`, `IM_LOAD_IMMEDIATE` | vertex and pixel shader microcode, hashed for the translated shader cache |
| `SET_BIN_MASK*`, `SET_BIN_SELECT*` | bin predication: predicated packets run for the selected tiles only |
| `DRAW_INDX`, `DRAW_INDX_2` | by `RB_MODECONTROL`: a draw (colour and depth, depth only) or an EDRAM resolve (copy mode) |
| `XE_SWAP` | the front buffer to present (what `VdSwap` writes) |

Memory follows the console: packets and constants are big-endian, and what the GPU writes or polls is little-endian,
swapped by the endianness in the low two bits of the address. Vertical blanks come from `sys::Graphics().StartVblank`,
which moves the GPU's counter (`Vblank`) and runs the title's callback (source 0).

The semantics are those of the reference command processor (Xenia's, which ReXGlue inherits): opcodes, the register
mask of `REG_RMW`, the fake query results and screen extents. ReXGlue's command processor cannot run without its
GPU, so it is not used as an oracle here. `gfx_cp_tests` checks each packet against those semantics instead, as
`sys_object_tests` does for the kernel objects.

## The Renderer

`Renderer` is what a graphics backend implements:

- `ShaderLoaded`: new microcode for one of the two shader slots.
- `Draw`: primitive type, index count and source (DMA indices with their address, length and endianness, immediate
  indices, or auto-generated), both shaders, and the register file at the time of the draw.
- `Copy`: a resolve, as the `RB_COPY_*` registers describe it.
- `Swap`: the front buffer address and size.
- `Invalidate`: memory the CPU changed, for texture and buffer caches.

## The Vulkan renderer

Vulkan is the one graphics API: Windows, Linux and the Steam Deck (the primary target: RDNA2 with RADV, 4 Zen 2
cores, 1280x800, so CPU cost per draw comes first and FSR is the upscaler that matters), Android, and macOS through
MoltenVK. WebGPU stays a possible side project for a browser build, not a second backend here.

### What comes from the Crackdown port

The Crackdown repository built the first native renderer, inside its patched ReXGlue GPU plugin (SDK patch 0129,
`src/hle_*`, `HLE_RENDERER_PLAN.md`). Its shadow atlas is drawn natively, per draw, in place of the emulated draws,
and equals them to the pixel (2026-10-30/31). What Any360 takes from it:

| Element | Where it was | In Any360 |
|---|---|---|
| Xenos microcode to SPIR-V translator (Xenia's, BSD-3): the shaders of the native atlas | ReXGlue `pipeline/shader/spirv_translator*.cpp` | vendored in `layer/gfx/third_party/xenos_shader`, without the rest of ReXGlue |
| Command processor semantics | ReXGlue `command_processor.cpp` | `CommandProcessor`, checked line by line against it |
| Draw records: the XDK D3D runtime's device state at each draw, in shared immutable blocks, with command buffer draws as records of their own | `src/hle_records.{h,cpp}`, `hle_frame_abi.h` | next: the record model and its expansion over the register file, for titles bound at the D3D level |
| PM4 walker for records and its tests | `src/pm4_decode.h`, `tests/pm4_decode_tests.cpp` | next, with the records |
| Per-draw matching of records to emulated draws (the comparison harness) | `src/hle_draw_match.h` + tests | next, as the renderer's own oracle against ReXGlue |

### Lessons that shape it

From the Crackdown runs (each one cost a debugging session there):

- **Hash shaders when the draw is recorded, not later.** The D3D runtime patches a vertex shader's microcode in
  place for each vertex declaration. Looking the shader up by address or hashing it at render time finds the next
  draw's variant: wrong vertex fetch, spiky geometry.
- **Dynamic vertex and index data belong to the draw's moment.** Skinned meshes are rewritten for the next frame
  once the GPU has consumed them. A renderer that reads them later (end of pass, end of frame) draws torn
  geometry. Copy them when the draw is recorded, or draw in stream order.
- **Command buffers hold most draws.** Precompiled buffers the game replays carried 2-4 times more shadow draws than
  the D3D calls. Their draws need records too, with the buffer's register and shader writes applied.
- **Passes resolve between cascades at the same EDRAM base.** A pass ends at its resolve, not at a marker.
- **Float24 depth**: host render targets need depth converted in the pixel shader to match the console (garage
  markers clipped otherwise); measured at ~0.5 ms on an RTX 5070 Ti.
- **Occlusion queries must be real host queries**: faked counts show markers through walls.
- **Render pass breaks dominate on Vulkan**: ~200 render pass entries a frame (barriers, attachment load/store at
  1440p with MSAA) cost more than the draws. Host render targets per surface avoid most of them.
- **Per-draw CPU cost is the budget**: the emulated path spent ~800-1,000 ns a draw in `IssueDraw` (bindings,
  textures, render targets). Dynamic uniform buffers and descriptor reuse cut it most.

### Plan

1. **Shaders** (done): the vendored translator, run on hand-built microcode in `gfx_shader_tests`, the SPIR-V
   compiled into pipelines by real drivers in `gfx_vulkan_tests`.
2. **Offscreen draw** (done, 2026-11-01): `VulkanRenderer` draws the command processor's draws. Two learned on the
   way: the alpha test's pass bits must be set for a disabled test (none set discards every pixel), and the colour
   exponent bias is a multiplier (1, not 0). Still narrow: one RGBA8 target, no depth, blending, culling, textures
   or resolves; the viewport without window offsets or scissors; rectangle and quad lists not expanded; the whole
   mirrored memory copied before each draw, one submission per draw; the translator's `Features` all on and its
   modification bits at their defaults rather than read from the device and the registers.
3. **Render state** (done, 2026-11-01): depth and stencil (front and back), blending and write mask of render
   target 0, culling and front face (D3D semantics checked: the `face` bit means clockwise front), wireframe fill,
   polygon offset in the depth format's unit, the scissor (window scissor with the window offset, within the screen
   scissor), as the reference reads them; dynamic state per draw. Depth is D24S8, or D32S8 where the device lacks it
   (the AMD driver). `gfx_render_tests` checks each feature to the pixel on NVIDIA and AMD.
4. **Render targets** (done, 2026-11-01): one host image per EDRAM surface the title binds, keyed by base tile,
   pitch (`RB_SURFACE_INFO`) and format (`RB_COLOR{,1,2,3}_INFO`, `RB_DEPTH_INFO`), sized to the rows the surface
   can span in EDRAM. Colour targets 0-3 are bound when `RB_COLOR_MASK` enables them and the pixel shader writes
   them; the depth/stencil target when its test is on. Host formats as the reference maps them; the exponent bias
   of `RB_COLOR_INFO` (with the 16-bit fixed-point formats scaled into -1...1) and the gamma conversion go through the
   system constants. Learned: `RB_BLENDCONTROL1-3` at 0 blend ZERO + ZERO, so a target the title leaves at 0 is
   written black (the reference does the same). Still open: MSAA, and surfaces that alias the same EDRAM in other
   formats or pitches (the reference copies contents between them; here each keeps its own).
5. **Textures** (first step done, 2026-11-01): 2D textures, base level, unsigned components, the formats a host
   image holds unchanged (8, 8_8, 8_8_8_8, 2_10_10_10, DXT1/3/5, 16- and 32-bit float), linear or tiled; samplers
   (filters, address modes, anisotropy, border); the translations now carry the interpolator mask the vertex and
   pixel shaders share (`SQ_PROGRAM_CNTL`), and pipeline layouts follow each stage's texture and sampler counts.
   Learned: the translator declares an unsigned and a signed binding per fetch and picks one at run time by the
   fetch constant's signs, so both must be bound. `gfx_render_tests` samples linear and tiled k_8_8_8_8 and tiled
   DXT1 to the pixel on NVIDIA and AMD. Next for textures: mips and the packed mip tail, signed views, cube, 3D and
   stacked textures, the formats that need conversion (5_6_5, 4_4_4_4, 10_11_11...), untiling on the GPU, and
   memory watching instead of hashing each use.
6. **Resolves** (first step done, 2026-11-01): a draw in EDRAM copy mode reads the rectangle of its three vertices
   (vertex fetch constant 0, with the half-pixel offset, 24.8 fixed point, within the scissor, aligned to 8 pixels,
   as the reference does) from the surface `RB_COPY_CONTROL` selects, converts it to `RB_COPY_DEST_INFO`'s format
   (red/blue swap, exponent bias, endianness) and writes it tiled to guest memory at `RB_COPY_DEST_BASE`, where
   the CPU and the texture cache see it; then it clears the colour and depth targets over the rectangle with
   `RB_COLOR_CLEAR` / `RB_DEPTH_CLEAR` decoded per format. `gfx_render_tests` draws a texture, resolves it with
   clears, checks every texel in guest memory and the cleared targets, and samples the result back, on NVIDIA and
   AMD. Depth resolves followed (depth << 8 | stencil, float24 for D24FS8), then resolves from MSAA surfaces
   (2026-11-02): the rectangle is in pixels and the surface holds samples (top-left, bottom-left, top-right,
   bottom-right in each 2x2 block at 4x; top and bottom at 2x); `RB_COPY_CONTROL`'s sample select takes one sample
   or averages two or four (8-bit components rounded to nearest, half floats as floats), made valid for the sample
   count as the reference does (depth takes one sample, never an average), and the clears cover the rectangle's
   samples. `gfx_render_tests` resolves a 4x surface three ways (averaged, sample 1, sample 2) and checks every
   texel, on NVIDIA and AMD. Next: more destination formats, 3D / array destinations, and host-to-host copies so a
   resolved texture need not go through guest memory.
7. **Present** (first step done, 2026-11-01): `XE_SWAP` presents the texture of fetch constant 0, as `VdSwap` (now
   native in any360::sys) writes it: the front buffer is read from guest memory as RGBA8 with its swizzle (the
   renderer's `frame()`, checked headless in `gfx_render_tests`), then the `Presenter` shows it in a window. The
   window comes from SDL3 and the platform layer makes the Vulkan surface from its native handle (Win32 for now,
   X11 / Wayland next), which also works with SDL builds that lack SDL's own Vulkan support (the reference SDK's).
   Learned: readback buffers need cached host memory (from 3 to 77 FPS on the first version).
   ALU opcodes (2026-11-04): `gfx_render_tests` runs every vector and scalar operation of the microcode
   (79 cases: the predicate setters, kills, previous-scalar and constant-scalar forms included) in a hand-encoded
   pixel shader on constants, exports the result to a k_32_32_FLOAT target and checks it against the semantics
   ucode.h documents; a killed pixel keeps the clear value. A scalar operation reads src3 as `a` = W, `b` = X.
   Control flow (2026-11-05): every opcode (nops, conditional execs on bools and on the predicate, their ending
   forms, loops, calls and returns, jumps, the vertex fetch hint) in the same bench; calls and returns, which the
   reference translator left unimplemented (a called function never ran), are now implemented (see the
   translator's NOTICE.md).
   Texture unit (2026-11-05): the gradients, the computed LOD, the register LOD and gradients a fetch then uses
   (the level it reads), the bilinear weights (D3D pixel centres: integer coordinates) on 128x128 two-level and
   32x32 textures. `getBCF` (the border colour fraction), which the translator and the reference left unimplemented (a translation
   error; the reference's note suggests sampling a black texture with a white border), is now worked out like
   getWeights: per axis, the magnification filter's taps (linear: the two texels either side of the coordinate less
   half a texel, by its fraction; point: the nearer one) and the weight of those outside the texture where the axis
   clamps to the border (ClampToBorder; MirrorClampToBorder mirrored about 0 first); 1 less the product of each
   axis' share inside; 3D's Z only for 3D data; cube maps 0. Like getWeights, at the base level (the LOD
   disregarded). Six bench cases: inside, an edge, a mirrored edge, a corner, a mirrored side inside, a point tap.
   Formats and primitives (2026-11-05): every vertex format through a vertex shader's fetch into an interpolator
   (unsigned, signed and integer forms, an exponent adjustment; components a format lacks read 0, a title's
   shader asking for 1 through the swizzle), every colour render target format but k_8_8_8_8 read back as the host
   stores it, D24FS8 depth, and triangle fans, polygons (a fan), quad strips (a triangle strip), line lists,
   strips and loops (a strip back to the first vertex, expanded like quad lists). The command processor now
   gathers an immediate shader load that wraps around the ring's end (it assumed it never would; the bench's
   ring did).
   Texture formats (2026-11-05): every format the reference's Vulkan texture cache loads, each sampled from
   a 4x4 texture of one value as a title stores it (linear rows 256-byte aligned, big-endian units the fetch
   constant's endian mode undoes): the 8-bit, 5:6:5, 6:5:5, 1:5:5:5, 4:4:4:4, 8:8:8:8, 2:10:10:10, 10:11:11,
   11:11:10, normalized 16-bit (signed too), half and 32-bit float, EXPAND, float24 depth, DXT1-5, DXN, DXT5A,
   DXT3A, CTX1 and 4:2:2 YUV ones, with the _A and _AS_16_16_16_16 forms. The texture cache gained, converting as the
   reference does: 1:5:5:5 (A1R5G5B5, red and blue exchanged), 4:4:4:4, 6:5:5 (rotated into 5:6:5, read back with a
   red-blue-green swizzle), the 16-bit normalized and EXPAND formats, DXN (BC5), DXT5A (BC4), 10:11:11 and
   11:11:10 (unpacked to 16-bit normalized RGBA), DXT3A (4-bit alpha to 8-bit texels), CTX1 (decoded to 8:8
   texels) and the 4:2:2 YUV formats Cr_Y1_Cb_Y0 and Y1_Cr_Y0_Cb (2x1 blocks, the loader's first blocks taller or
   shorter than wide, decoded to RGBA8 texels of Cr, Y, Cb, as the reference's fallback does). 5-bit components reach the sampler through 8 bits (bit replication). Not handled, as by the reference:
   the 1-bit, _B, 32-bit integer, _AS_8, MPEG, interlaced, 32_32_32_FLOAT and EDRAM formats.
   Render features (2026-11-05): back-face stencil, blend constants, the alpha test, wireframe fill, polygon offset,
   the window offset, MSAA 2x as samples, user clip planes, linear texture filtering, vertex shader textures and
   constants addressed by a0, each a scenario. Two were wrong: the alpha test compared with 0 (RB_ALPHA_REF never
   reached the shader), and user clip planes were ignored (PA_CL_CLIP_CNTL and PA_CL_UCP_n now set the vertex
   shader's clip or cull distances, as the reference translates them).
   Texture dimensions (2026-11-05): stacked 2D layers (tfetch3D at normalized w, slices 4 KB-aligned), cube faces
   (tfetchCube at face coordinates) and now 3D textures: a 3D host image whose z slices the cache loads at the row
   pitch times 32 rows (the volume, depth aligned to 4, is what aligns to 4 KB) or, tiled, through the 3D tiling
   (XGRAPHICS::TileVolume, as the reference computes it); the base level only for now.
   Points (2026-11-05): point lists drawn as two-triangle strips, each the square of PA_SU_POINT_SIZE (or of the size
   the vertex shader writes, clamped by PA_SU_POINT_MINMAX), the vertex shader expanding them as the reference does
   without geometry shaders (the renderer drew sizeless host points). Point sprite coordinates (pixel parameter
   generation on points, which the renderer refused) now come from that expansion: S and T 0 to 1 across the
   square, rightwards and downwards, as the reference passes them. Anisotropic filtering (a 4:1 fetch constant keeps the finer level of a
   texture read obliquely) and bin predication (gfx_cp_tests) are tested too.
   Alpha to coverage and occlusion queries (2026-11-05): RB_COLORCONTROL's alpha_to_mask_enable and dither offsets
   never reached the shader. Guest samples are host pixels here, so the translator (an any360 mode, as the reference
   does it per host sample) finds a fragment's guest pixel and sample from its position and discards it when alpha
   is below the sample's threshold (1x 1, 2x top 0.5 / bottom 1, 4x TL 0.75 / BL 0.25 / TR 0.5 / BR 1) less the
   pixel's offset; gl_SampleMask is no longer declared, so the host's own MSAA keeps every sample of a kept fragment.
   Scenarios at 1x, 2x and 4x. Occlusion queries, already counted by host queries, got their scenario: a full quad
   reports every sample and one the alpha test discards none.
   Centroid interpolation (2026-11-05): the pixel shader's interpolators are decorated Centroid as the reference
   picks them (guest MSAA, SQ_CONTEXT_MISC sc_sample_cntl, SQ_INTERPOLATOR_CNTL's pattern), part of the shader key.
   Guest samples being host pixels, it matters to the host's own MSAA (no extrapolation at partly covered edges);
   tested for the choice and for a 2x draw through it, not for the host MSAA image itself.
   Immediate indices (2026-11-05): the indices a DRAW_INDX or DRAW_INDX_2 packet carries itself (source select 1),
   which the renderer refused and the reference does not draw, are staged in the batch's index buffer and drawn as
   DMA indices of host order: two 16-bit indices a dword, the first in the low half, as an R6xx-family VGT takes
   them (Mesa's r600 driver copies little-endian user indices into its immediate draw packets); no Xenos source
   confirms it, and no known title emits them. Rectangle and point lists (their vertex shader loads guest indices
   from memory) still refuse them. Scenarios: a 16-bit triangle (its half order checked), a 32-bit quad list and a
   strip with primitive reset.
   2D primitives (0x10-0x16: copy rect lists V0-V3, fill rect list, 2D line and triangle strips) stay refused, for
   want of a source: the reference refuses them, Mesa's AMD and Adreno register files and RADV only name them, and
   AMD's R6xx/R7xx 3D Register Reference (2009) lists them by name only (its VGT_GROUP_PRIM_TYPE grouper types
   VGT_GRP_2D_COPY_RECT_V0-V3 / FILL_RECT / LINE / TRI likewise; PA_SC_LINE_CNTL's Bresenham control is "set for 2D
   lines"). Their vertex counts, what V0-V3 differ in and whether they bypass clipping and the viewport transform
   are not documented there; no known title draws them.
   Memory export (2026-11-05): the translator already wrote memexport streams into the guest memory buffer, but
   nothing brought them back. The renderer now gathers each draw's streams as the reference does (the stream
   constants the shaders' `mad eA` read, checked by their marks; a range of count x element size each), submits the
   batch with them and waits: guest memory in video memory (the default) copies the ranges into a readback buffer
   and then into guest memory, a mirror copies them back, imported memory has them in place; the texture cache
   forgets those ranges. Rare and synchronous by design (later draws and the title's code may read the results).
   The ranges are uploaded to the GPU's copy first: they come back whole, and elements the shaders did not write
   would otherwise undo what the title wrote there since (a scenario caught it).
   Without rasterization (EDRAM mode other than 4 or 5, no target bound, an empty scissor: the "compute shaders"
   titles make of point draws covering nothing, which the renderer skipped), a memory-exporting vertex shader runs
   as a compute dispatch of the translator's memexport translation (kMemExportCompute, one invocation a guest
   vertex, auto or DMA indices), outside the render passes; the reference's Vulkan backend draws it with
   rasterizer discard instead and never used that translation, whose position export crashed the translator
   (fixed, see the translator's NOTICE.md). Not yet: immediate indices, vertex shader textures there. Scenarios: a
   vertex shader writing a 32_32_32_32_FLOAT element per point, drawn, without rasterization, and from DMA indices
   {2, 3} (elements 0 and 1 untouched); the video memory path (mirror and imported paths untested).
   PM4 packets left (COND_EXEC, MEM_WRITE_CNTR, EVENT_WRITE_CFL, DRAW_INDX_2_BIN, SET_STATE, SET_BIN_BASE_OFFSET,
   IM_STORE): no layout to implement them from. The reference names them only (xenos.h, one-line purposes);
   Mesa's adreno_pm4.xml gives the same one-liners, and its CP_COND_EXEC layout (64-bit addresses: bool, timestamp,
   active timestamp, dword count) is the later Adreno one; no a2xx or Xenos code emits or parses any of them. None
   is in the Crackdown capture (its packets: SET_BIN_MASK/SELECT, DRAW_INDX, DRAW_INDX_2, SET_CONSTANT,
   LOAD_ALU_CONSTANT, IM_LOAD, IM_LOAD_IMMEDIATE, EVENT_WRITE, _EXT, _ZPD, _SHD, WAIT_REG_MEM, INDIRECT_BUFFER,
   INVALIDATE_STATE, INTERRUPT, XE_SWAP, REG_RMW). The command processor skips them by their length, as before.
### Speed (2026-11-01)

Measured with `gfx_window_demo --draws N` (N triangles, each with its own vertex buffer and constants, a textured
1280x720 quad, a resolve, a swap, a present per frame), per frame:

| Step | 2,000 draws before | After |
|---|---|---|
| Guest memory to the GPU | 381 ms (8 MB copied per draw) | 0 (imported in place) |
| Draw recording and submission | 178 ms (a submit and a wait per draw) | 2.5 ms (one batch) |
| Resolve | 6.0 ms (texel by texel) | 1.5 ms (4-texel runs, threads) |
| Swap | 6.0 ms (read back, untiled, swizzled on the CPU) | 0 (presented from the resolved surface) |
| Frame rate, RTX 5070 Ti | 2 FPS | 214 FPS |
| Frame rate, Radeon integrated | | 153 FPS |

- **Guest memory in place**: with `VK_EXT_external_memory_host` (NVIDIA, AMD, RADV; 4 KB alignment) the guest's
  physical memory is imported as the storage buffer the translated shaders fetch from; nothing is copied. Without
  it, the mirror is copied once per batch.
- **Batches**: draws are recorded into one command buffer, the render pass kept open while the targets stay the
  same; constants, indices and texture descriptor sets come from rings and pools reset at each flush. The batch
  runs before anything the CPU can observe of the GPU's progress (fences, memory writes, scratch registers,
  interrupts, the read pointer: `Renderer::Flush`, called by the command processor), so a title never rewrites
  memory pending draws still read; resolves, swaps, clears, readbacks and texture uploads flush first.
- **Swap from the GPU**: when the front buffer is exactly the last resolve's destination (same address, format and
  endianness, colour components unswizzled), the frame is that resolve's source surface, blitted to the window
  without any readback; otherwise it is read from guest memory as before (both paths tested).

What remains on the CPU: the resolve's readback and tiling into guest memory (needed while the CPU or a later
texture can read it), texture untiling and hashing, and translating shaders the first time.

### The CPU's share in play (2026-11-12)

`tools/cpu_profile.ps1` on Crackdown's city (45 s, every sys group and the GPU on Any360, the native library
functions of docs/NATIVE_FUNCTIONS.md bound): the process used 1.33 processors; Any360's graphics took 15% of its
cycles on three threads:

- **The command processor's thread (6%)**: `ExecutePacket` 3.5%, most of it type-0 register writes (Crackdown
  sends 5 to 8 MB of registers a frame, its constants for 2,000 draws: a byte-swapped copy of that much is mostly
  memory traffic, the swap itself 0.1-0.2 ms a frame), then `DRAW_INDX`. Its WAIT_REG_MEM (16 ms a frame: the ring
  waits for the title's frame pacing word) sleeps on the progress count and costs little.
- **The renderer's thread (6%)**: `Draw` 2%, texture loads, `MemoryChanged`.
- **The completion thread (2.8%)**: the resolves written back into guest memory (`FinishWriteBacks`: 21 a frame in
  places), with `TiledOffset2D` and the byte shuffle called once per 16 bytes.

The write-back is now `WriteBackTiled` (`layer/gfx/src/vulkan/write_back.cpp`). A tiled offset is that of the same
element in the row's first 32 elements plus a step per 32 (the macro tile's x lands in the offset's bits from 9 up,
which the tiling only shifts), so a row needs a table of 8 or 16 offsets instead of one computation per 16 bytes;
the byte shuffle runs in the loop instead of a call (it was a function of its own: SSSE3 code cannot be inlined
into the baseline build's function); the depth conversion to float24 and the unorm rounding are inlined, lround
replaced by a rounding in double that gives the same values. The first version stays as
`WriteBackTiledReference`: `gfx_write_back_tests` compares the two on 3,000 random rectangles (both element sizes,
every byte order, depth unorm and float24, any offset and width, past the end of memory) and, with `--bench`,
times them: a 1280x720 colour resolve 0.93 -> 0.37 ms (2.5x), depth 2.6 -> 1.9 ms (1.4x).

In game, `ANY360_WRITE_BACK_BENCH=1` writes each resolve back both ways (the same bytes twice, in alternating
order) and the log's "written back later" gives both times. Crackdown's city: 0.51 ms a frame against 0.98-1.01
with 21 write-backs a frame, 0.09 against 0.19 with 3: 2.0x, about 30 ms of a processor a second where the title
reads back most.

The renderer's thread (20% of the cycles in the city): `Draw`'s own code is spread over hundreds of lines, none
above 0.2% of the cycles (`tools/cpu_profile.ps1 -LinesOf 'VulkanRenderer::Draw'`, a new option: a function's
cycles by source line, and the time its calls spend in DLLs by the line that called). What stood out:

- **The diagnostics' clock.** Every other report period times each draw's sections (`Timer`, `mark`), and
  steady_clock's QueryPerformanceCounter cost 1.25% of the process's cycles. The renderer's timers read the
  time-stamp counter now, in nanoseconds by a rate measured once at `Initialize`: 0.18%. The report reads truer
  too: "draws" was 4.2-4.3 ms a frame for 4,300 draws and is 2.8-3.2 ms for 5,100 (the clock was inside what it
  timed).
- **Images made and freed in Crackdown's cut-scenes.** The log's report gives the GPU objects created a frame now
  (`gfx::vk::CreatedGpuObjects`), and `ANY360_LOG_ALLOCATIONS=1` logs each with its size, format and caller. Before
  the city, Crackdown resolved three formats to one address: a resolve kept one spare image of another format, so
  each switch freed one and made one, 6 images a frame for about 40 seconds (their creation and release on the
  completion thread were 2% of the cycles where it ran). A resolve keeps up to four spares now
  (`Resolved::spares`, the oldest freed beyond them): 0.00 images a frame in the same stretch. In the city's steady
  scenes nothing is created either way.
- Left as they are: the constants' compare and copy into the batch's buffer (about 1% between them: the buffer is
  the GPU's, written once; the copy kept to compare against is the cached one), and the record threads woken every
  32 draws (0.9%).

### Frame capture and replay (2026-11-01)

Hand-written PM4 shows that a feature works; only a title's frames show which features it needs, in what order.
A capture ([CAPTURE.md](CAPTURE.md)) records what a GPU front end ran: the register file when it starts, then for
each kick of the ring the physical memory pages that changed and the ring's span, and the reference's presented
frame at each swap. `gfx_replay CAPTURE` runs it through our command processor and Vulkan renderer and reports the
draws not drawn, by reason, the PM4 packets the frames use, and how far each frame is from the reference's
(`--out DIR` writes both as PNG). Captures hold the title's data: they stay on the machine that made them.
`--trace` prints the frames' structure (runs of draws by render target with their outcome, each resolve, each swap)
and `--dump-resolves DIR` writes each k_8_8_8_8 colour resolve as the title will read it.

First Crackdown capture (city, 3 frames, 244 MB): about 7,000 draws a frame. Pixel parameter generation was the
first gap (4,894 draws refused, now handled), then float24 depth clears (handled). The geometry passes now draw:
the deferred renderer's normal and albedo targets come out right in the resolves. Depth resolves (the EDRAM word: depth << 8 |
stencil, float24 for D24FS8) and k_24_8 / k_24_8_FLOAT textures (sampled as float depth) then gave the first lit
image: sky, buildings, the character, the HUD. The geometry stretched toward the camera came from primitive reset
(strips cut at VGT_MULTI_PRIM_IB_RESET_INDX) and the index offset and range (VGT_INDX_OFFSET, VGT_MIN/MAX_VTX_INDX),
now applied as the reference does: the frame's composition matches the reference's (mean difference 29 per channel
on the first frame, from 108). Rectangle lists (the shadow maps' clears; expanded in the vertex shader) and quad
lists (the HUD's text, particles; drawn as triangle lists) followed. Still missing there: the ground and shadowed
areas. Stacked textures (the sun pass reads the two shadow cascades as one), cube textures (726 draws of the main
pass) and k_5_6_5 textures (771) then brought the refused draws to 238 of about 12,700 (MSAA surfaces, signed
textures, the packed mip tail): the car, the rifle, the far buildings appear. The ground is still dark, now
without a refused draw. Diagnosed with --trace, --dump-resolves and switching depth resolves off: the frame starts
with a colour and depth clear drawn on a 4x MSAA view of the same EDRAM (pitch 640 for the 1280-wide surface), which
was refused, so from the second frame on the main depth kept the last frame's end state and the ground failed its
depth test. MSAA surfaces are now drawn as their samples (a 4x surface of pitch P is the EDRAM of a 1x surface of
pitch 2P with twice the rows; 2x doubles the rows), the coordinates scaled to samples: the clear reaches the main
surfaces and the three frames match the reference's composition (mean difference 27 per channel each; the rest is
mostly texture noise without mip levels). Mip levels then (the fetch constant's level range, levels 1 and up at the
mip address, the packed mip tail's offsets, as texture_util lays them out): the grain is gone and the frame is close
to the reference's (mean difference 21.7 per channel). Signed textures then (a signed view of the same bits,
SNORM, bound to the shader's signed binding when a component is signed): every draw of the capture is drawn (12,857,
none refused, NVIDIA and AMD). What is left of the difference (21 per channel) is uniform: ours is about 17 brighter
in each channel, along a monotonic curve (0 stays 0, mid-tones darker: 136 against 114, white close to white): the
display gamma ramp the reference applies when it presents. The command processor now keeps the ramp from the DC_LUT
writes as the reference does (the 256-entry table and the PWL ramp) and hands it to the renderer at each swap, whose
frame reads go through the table (k_8_8_8_8 front buffers); captures record the ramp (kGammaRamp), since the title
writes it long before a capture starts. Second capture (2026-11-02, with the ramp recorded; 14,518 draws, none
refused): the brightness bias is gone (mean signed difference -0.6 per channel, from +17), the mean difference is 9.6
per channel (from 21) and the frame matches the reference's by eye, the beam effect included; what remains is
slightly softer distant textures (anisotropic filtering or LOD bias, likely). The window path applies the ramp too: a GPU frame goes through a compute pass
(the frame's surface, the ramp as a 3 x 256 LUT, an intermediate image, then the blit; its SPIR-V is built in code
with the vendored glslang builder, so no shader compiler is needed), and CPU frames go through the LUT at the swap.
What looked like softer distant textures was an offset: the per-pixel difference sat on every edge, the HUD's
included, and was smallest with our frame moved by one pixel. D3D's pixel centres (PA_SU_VTX_CNTL pix_center 0) are
at integer window coordinates; the reference moves the window coordinates by half a pixel then, which the renderer
now does too: on the second capture, mean difference 0.09 per channel, 0.21% of pixels off by more than 8 (NVIDIA
and AMD), from 9.6 and 31%. Resolves from MSAA surfaces followed (step 6); neither capture has one, and both
replay as before. The first capture (hle_gpu_49, recorded before the ramp was) stays at 17 per channel: the ramp is
missing, not a feature.

8. **Records**: the Crackdown record model, so a title bound at the D3D level skips PM4 entirely.
9. **Binding** (first step done, 2026-11-02): a title still built on the ReXGlue runtime reaches any360::gfx
   through any360::sys, graphics first (`layer/host/rexglue`, `any360/host/rexglue.h`); the rest of the kernel
   stays the runtime's until it moves too, the end state being the whole title on Any360's layers.
   - The title's GPU system calls (`VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`,
     `VdSetGraphicsInterruptCallback`, `VdSwap`, the system and scaler command buffers, the engines) are overridden
     in the title's executable (`ANY360_REXGLUE_GRAPHICS_EXPORTS()`): the generated code calls `__imp__Vd*`, which
     the title's own definition takes from the runtime's import library. Each override calls `any360_Vd*` once
     the graphics are bound, the runtime's export (looked up in its library by name) otherwise, so one build runs
     either way. `VdPersistDisplay` and the display-mode queries stay the runtime's (guest memory is still its own).
   - The GPU MMIO range the runtime registered (0x7FC80000) is taken over in place (`MMIOHandler::LookupRange`
     hands out the range), so the title's stores to `CP_RB_WPTR` reach `GraphicsKernel::WriteRegister` and the
     command processor. No SDK patch is needed for either.
   - The title's interrupt callback calls the runtime's kernel, which needs a runtime thread: `GraphicsKernel` got an
     interrupt dispatch (`SetInterruptDispatch`), and the binding runs the callback on a runtime host thread through
     `ExecuteInterrupt`, synchronously for the thread that raised it (the command processor's, the vertical
     blank's), as the reference does.
   - `StartGraphics` makes a Vulkan device presenting to a plain Win32 window of its own (the runtime has its own
     SDL inside its library), the renderer over the guest's physical memory, the command processor (on its worker
     thread), and starts vertical blanks; each swap is presented on the command processor's thread.
   `host_rexglue_tests` runs it inside the reference runtime (tool mode): before the binding the overrides reach the
   runtime's exports; after it, a ring set up through the Vd* calls and kicked through the MMIO handler runs, its
   read pointer is written back, and the title's callback (a guest function) runs on a runtime thread for the
   ring's INTERRUPT and for vertical blanks, while the placeholder range is never reached. Next: wire it into the
   Crackdown build behind a setting and play a frame.

It avoids geometry shaders and rasterizer-ordered views so that it runs through MoltenVK and on mobile GPUs:
rectangle and quad lists are expanded in the vertex stage.

### Lost devices (Call of Duty: Black Ops, 2026-11-12)

Call of Duty's levels reset NVIDIA GPUs after a few minutes (System log: nvlddmkm event 153 twice, 2 s apart) on
any360::gfx and on ReXGlue's own Vulkan backend alike, never on its Direct3D 12 one: the shared SPIR-V translation.

- **Forward jumps stay in one pass.** The translator made every `jmp` a new pass of its main loop (`loop { switch
  (pc) }`). In a quad where some pixels jumped and others did not, the pixels reached the code after the label in
  different passes, and derivatives taken there (texture LOD) waited for pixels that never came: the GPU hung,
  seemingly at random, in the world materials whose shadow lookups are skipped by a predicated `jmp`. A forward jump
  to the next label is now a selection within the same pass (`xe_var_jump_skip`); backward jumps, calls and loops
  keep the main loop. Bisected in game: emptying the 3D, cube or shadow textures did not help, sampling without
  derivatives did, and the change held 10 minutes where six runs before had frozen in 1 to 5.
- **Shared memory addresses are taken modulo 512 MB** (a vertex fetch or memexport past the buffer is undefined in
  Vulkan; Direct3D reads 0).
- **Diagnostic**: `ANY360_GPU_CHECKPOINTS=1` marks every draw for the GPU (`VK_NV_device_diagnostic_checkpoints`);
  after a lost device the stall log names the draw each pipeline stage last passed, with its shaders, index range,
  vertex buffers and textures.

Tests: `gfx_shader_tests` (the jump's selection, the address mask), `gfx_render_tests` (control flow cases, a
vertex fetch past 512 MB drawn from the wrapped address), `gfx_render_tests_checkpoints` (every scene with the
checkpoints on), and `gfx_resource_tests`: 3,000 frames of textured draws, uploads, textures used once, a resolve
and a swap, checking that nothing the renderer holds grows without bound and the process stops growing; it reports
CPU and GPU time a frame as the baseline for optimizations (RTX 5070 Ti: ~0.3 ms CPU, ~0.02 ms GPU, textures capped
at 1,207 by eviction, +4 MB from frame 1,800 to 3,000).

### Pipeline warm-up (2026-11-12)

Vulkan wants a pipeline for each combination of shaders and render state, and the renderer only learns a combination
at the draw that needs it; with asynchronous pipelines that draw is left out until its pipeline is made. The driver's
cache (`any360_pipelines.bin`) keeps pipelines for the settings they were made with, so a new host MSAA or a new
driver starts empty: Call of Duty's first run at 4x MSAA left 847 draws a frame out while it compiled, objects
appearing only once approached, against 3 to 29 in runs whose pipelines were cached.

Every pipeline a title draws with is now also kept as a recipe beside the cache (`<cache>.recipes`): the shaders'
microcode, their modifications, the render state and whether the pass was multisampled, not the host MSAA or the
resolution. At the next run the recipes are compiled for that run's settings before the title draws with them
(`WarmUpPipelines`: each swap translates what 2 ms allow, the compiler threads make the pipelines; a recipe
recorded without host MSAA is made at one sample and at the host's count). Call of Duty: 624 recipes from a 1x run,
then a first run at 2x with 2x MSAA, a setting never used: no draw waited. A title may call `WarmUpPipelines` with a
larger budget and `FinishPipelines` in its loading screens (a fine-tuning step, after the generic path).

Test: `gfx_render_tests` records two pipelines at one sample, warms them up at 4x MSAA in a new renderer, and its
first frame makes none and waits for none.

### EDRAM shared between surfaces (2026-11-12)

EDRAM is one memory, and a title may draw a target at one base and read it back from another: Call of Duty: Black
Ops draws its 1024x2048 shadow atlas in two halves at depth bases 0x180 and 0x4C0 (pitch 1040, 13 tiles a row: the
second is the first's rows 1024 on) and resolves it whole from 0x180. With one host image per base the second half
never reached the resolve, so the shadow test said "lit" there (stairs and walls bright where the reference is dark).

Surfaces of the same pitch, format and kind whose bases are a whole number of tile rows apart now share an
EdramGroup: for each tile row of EDRAM (16 sample rows), the surface that wrote it last and the version. Every write
(a draw's rectangle, a resolve's clear) marks its rows; before a surface is drawn, resolved, cleared in part or read
back, the rows another surface wrote since are copied from that surface (its samples resolved, kept), whole rows
(the writer held the rest of each row: it was synced before it wrote). A first version copied everything the other
surface had ever written and brought another frame's scene depth into a lamp's shadow map piece (the player's gun in
its light); tile rows by version cannot.

Depth resolves kept on the GPU stay at the guest resolution whatever the resolution scale: a title samples a depth
texture texel by texel (a shadow map's percentage-closer filter with offsets of one guest texel and getWeights2D's
weights on the guest grid), and scaled, light leaked along shadow edges in thin streaks.

Tests: `gfx_render_tests` (red at base 0, green at base 2 read from base 0 from row 32 on; a top-half redraw keeps
the green; newer rows only; at scale 2 a depth resolve held at 1x and a colour one at 2x). Measured on a reference
capture of Black Ops's spawn view: the stairs' gap to the reference from 17.1 to 0.56 (1x) and 0.74 (2x).
Still open there: a lamp's spot light whose lit area is cut to a rectangle (its own 512x512 shadow map at 1D5D0000,
drawn and resolved at one base: not this), and light sprites (a floor lamp's halo).

### Pixel positions over guest MSAA, scaled slope bias (2026-11-13)

A guest MSAA surface is drawn as its samples (2x: twice the rows; 4x: twice the columns too), so a fragment's
coordinates count samples. The pixel position a pixel shader asks for (SQ_PROGRAM_CNTL param_gen) is the guest
pixel's again: the translator halves the sample coordinate along each doubled axis (keeping the resolution scale's
fraction, as the reference does at scale). Before, rows were twice too far down: Call of Duty: Black Ops's soft
light halos read the scene's depth at the wrong place and showed as their sharp-edged quads (a lamp over a door).
Black Ops's spawn reference capture: 1.91% to 1.10% of pixels off.

PA_CL_CLIP_CNTL clip_disable is not honoured yet: the reference clamps depth then (depthClampEnable). Any360 did for a
day; no Black Ops reference capture changed with it, and an in-game report of dark walls first blamed on it was the
scaled post-processing below, so it stays out until a capture shows a draw that needs it.

### Data passes at a resolution scale: 1:1 fetches at the guest pixel's centre, half-pixel fill (2026-11-13)

Black Ops builds a colour grading table each frame: a full-screen pass reads a 32x1024 identity table (a texture at
the guest's resolution) one texel a guest pixel with linear filtering and draws the graded 32x32x32 table, which a
later pass reads back as a 3D texture. At 2x each host pixel sampled a quarter of a texel off the console's sample
point, blending neighbouring entries (across the table's slices too); averaged back, black came out 31,24,28 and
dark debris showed flat lavender where the reference is black. Taking one host pixel instead of the average (tried
first) gave other entries of the wrong block: coloured rings in gradients (a TV's glow, floor lights).

- In a pixel shader at a scale above 1, a 2D fetch of a texture at the guest's resolution whose coordinate moves one
  texel a guest pixel along the screen's axes (within 1/64) is made at the guest pixel's centre: every host pixel of
  it gets the console's value. Other fetches (scene textures, magnified or minified) keep the host's detail.
- As the reference does (resolve_resolution_scale_fill_half_pixel_offset), a resolve of a scaled surface drawn with
  D3D's pixel centres gives the first scale/2 host rows and columns of its area the first covered one's values: the
  half-pixel move leaves them uncovered for quads on whole pixels.
- Scaled resolves written back stay averaged; with the above the average is the console's value for such passes.

Reference captures at 2x with 4x host MSAA, pixels off before / after: debris 7.24% / 1.10%, spawn 3.39% / 2.16%,
lamp 8.01% / 5.21%, Pygmalion wall 4.71% / 2.70%. Test: `gfx_render_tests` (at 2x, a black and white column texture
read 1:1 with linear filtering, resolved and written back: every texel exact).

Polygon offset's slope factor is multiplied by the resolution scale (the guest's unit is 1/16 of a guest pixel; a
host pixel is a fraction of one), as the reference does: less bias at 2x let shadow maps fight with their receivers.

gfx_replay `--dump-resolves` now writes a resolve kept on the GPU into guest memory first (WriteBackForTools), so
depth resolves dump what the replay made instead of zeros.

Tests: `gfx_render_tests` (parameter generation at 2x and 4x MSAA, every sample its pixel's position; at scale 2 a
plane biased by its slope in front of the same plane).

### New render targets and resolve images start as zeros (2026-11-13)

A render target the renderer creates (and its host MSAA image) and a new resolve image are cleared to zeros, as the
console's EDRAM holds before anything is drawn there, instead of what video memory held: a title blending over a
target, or a texture reading part of a destination, before the first clear or resolve showed that as flickering
colours (Black Ops at the start of a match, where its targets are created). The test API's ClearColor and
ClearDepthStencil take the surface before flushing, so its zeros come first. Test: `gfx_render_tests` (three fresh
targets read back as zeros). The reference captures replay unchanged.

### Sync validation of the replays; a frame burst for flicker (2026-11-13)

The zero clears of new images were followed in the same layout by copies into them with no barrier (write after
write): `Transition` adds none when the layout does not change. A barrier now follows the clears. Found with the
Vulkan SDK's validation layer and its synchronization validation over gfx_replay:

    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 VK_LAYER_PATH=<SDK>/Bin
    gfx_replay CAPTURE --scale 2 --msaa 4 [--async --threads 4]

Black Ops's four reference captures replay with no validation error, at 1x and at 2x with 4x host MSAA, with and
without the asynchronous renderer and parallel recording.

The rexglue host's screenshot option records a burst: when `<screenshot>.burst` appears (its text: the frame count,
900 if empty), the next presented frames are written at half size as raw PPM files into `<screenshot>_burst` by a
thread of their own (frames are dropped when it falls behind, and the log says how many). A light missing for one
frame at the start of a Black Ops match was found that way; a capture slows the title too much to show it.

### One thread at a time on the queue (2026-11-13)

The renderer's submitter thread, the presenter (its submit and present, and vkDeviceWaitIdle when the window is
resized), one-time submits and the destructors all used the one Vulkan queue with nothing between them; Vulkan wants
a queue (and vkDeviceWaitIdle, which uses every queue) used by one thread at a time. At a title's start, when the
embedded window follows the title's and the swapchain is made again, the resize's vkDeviceWaitIdle met a frame's
batches being submitted: Black Ops lost the device (or hung a batch, or crashed in the driver) in the first seconds of
about one launch in four at 1440p with 4x MSAA. `Device::queue_mutex` is now held around every use of the queue.
Launched 15 times in a row after it: no failure (4 of 15 the run before). The Vulkan SDK's crash diagnostic layer
showed the way: every submit had completed when the device was lost.

Test: `gfx_render_tests` (one-time submits on a thread of their own beside 100 frames the renderer draws and submits:
every frame right, no submit failed).

### Implicit-LOD sampling; draw time by pixel shader (2026-11-16)

On a small GPU (an AMD Radeon iGPU with 2 compute units, the class of the target devices) Crackdown's city is bound
by the GPU: 45 FPS, 18.3 ms of draws a frame. To see which draws cost it, `ANY360_DRAW_TIMING=1` now also puts the
draws' GPU time by pixel shader in the in-game report (`Any360: GPU draw ms per frame by pixel shader`, the 12
costliest, each with its draws and time a draw), as `gfx_replay` already printed it for a capture. The costliest:
a world shader (4.6 ms, 339 draws), a full-screen shadow pass (2.7 ms in one draw: 12 `tfetch3D` of the shadow map a
pixel) and two other full-screen passes (1.8 and 1.2 ms).

The translator sampled every pixel-shader fetch with the computed LOD with explicit gradients (the coarse derivatives
of the coordinates scaled by 2^LOD, `OpImageSampleExplicitLod ... Grad`). An implicit-LOD sample biased by the same LOD
(`OpImageSampleImplicitLod ... Bias`) selects the same level (log2(|g| 2^b) = log2|g| + b) and hands the texture units
one operand instead of two gradient vectors: 3 address dwords against 9 for a 3D fetch. `Features::implicit_lod_sampling`
(off by default, the reference's translation; the renderer turns it on) does that for computed-LOD fetches without
register gradients; `ANY360_EXPLICIT_GRADIENTS=1` goes back to the gradients for comparisons. The derivatives are taken
where the gradients were (coarse, so per quad), in the same control flow.

- Images: a capture of 29 city frames replayed both ways: 0.04% of the pixels differ on the Radeon, 0.10% on NVIDIA, by
  1/255 at most (the replay is deterministic: the same way twice, 0 pixels differ).
- GPU time on that capture: Radeon 538.7 -> 512.7 ms of draws (-4.8%; the shadow pass -12%), NVIDIA 25.6 -> 23.7 ms
  (-7%; the shadow pass 125 -> 76 us).
- In game on the Radeon, 5 launches a way alternating: 46.4 -> 48.5 FPS (median 46.2 -> 48.0), 10% low 45.9 -> 47.7,
  1% low 45.1 -> 46.8 (rank test z -5.6, -5.6, -6.1); the dips unchanged (1% low 42.4 -> 43.7, not significant);
  private memory 2672 -> 2669 MB. Archived in `docs/perf/archive/2026-11-16-amd-igpu/`.

Tests: `gfx_shader_tests` (a 2D, a 3D-or-stacked and a cube fetch: every sample implicit with the LOD as its bias; with
register gradients, every sample keeps them; without the feature, every sample has gradients), `gfx_render_tests`
(mip level selection and the textured draws with the renderer's setting).

Crackdown takes `--any360_gpu_capture=<file>` (creating `<file>.go` records the next `--any360_gpu_capture_kicks`
kicks) to capture a frame in game for `gfx_replay`.

### Resolves sampled in place (2026-11-16)

A texture over a resolve kept on the GPU was copied out of the resolve's image into an image of its own each time
the resolve changed: about 24 MB a frame in Crackdown's city (four 1280x720 colour and depth resolves, the two
1024x1024 slices of the shadow map, smaller ones), 1.3 ms of the iGPU's frame. On the city capture, skipping every
such copy (timing only) saved 5.6% of the GPU time and skipping those of single-slice textures 4.5%; the copies
were bytes, not barriers (narrowing their barriers saved nothing).

- **In place.** A one-slice texture of exactly the resolve image's size, in its format or its red/blue swapped one,
  samples the image itself: `TextureCache::SetSourceView` asks the renderer for a 2D-array view of it (by format and
  components, the swap undone in the components), and the renderer puts the image in SHADER_READ_ONLY_OPTIMAL before
  the draws recorded next. Its views are retired with the image (`RetireResolveImage`: a scale change, an evicted
  spare, growth, a containing image), and such a view is never kept by fetch constant (`last_view_direct`), the
  image's layout changing with the next resolve or write-back; a write-back puts a sampled image back in
  SHADER_READ_ONLY_OPTIMAL after its copy. Copies out of a source (`SetSourceForCopy`) put it in TRANSFER_SRC_OPTIMAL
  first. Signed views of another format, stacked slices and textures narrower than the pitch are still copied.
- **Exact heights.** Resolve images were as tall as the resolved rows rounded up to 32 (1280x736 for 720 rows), so no
  texture matched one. They are now as tall as resolved; a texture reaching to the end of the last 32-row tile is
  still copied, the rows below the resolve cleared to zeros (what the taller image held).
- A growing resolve image's copy of its earlier rows was followed in the same layout by the blit into it with no
  barrier (write after write); synchronization validation found it once images grew more often, and a barrier now
  follows the copy.

Measured on the Radeon iGPU (cores 8-15):

- Capture replay: 626.9 -> 603.3 ms of GPU time for 29 frames (-3.8%), texture copies 38.4 -> 14.4 ms; before and
  after give the same images (0 pixels differ on the Radeon and NVIDIA, at 1x and 2x); 0 synchronization errors on
  both GPUs (1x; 2x with the asynchronous renderer and 4 threads; 2x with 4x MSAA).
- In game, 5 launches a build alternating: 47.8 -> 49.6 FPS (median 47.4 -> 49.4), 10% low 47.1 -> 48.9, 1% low
  46.1 -> 47.9 (rank test z -5.1, -5.1, -5.5); private memory 2687 -> 2652 MB (the copies' images); the dips
  unchanged (1% low 44.3 -> 44.8; their slowest frame 32.1 -> 42.5 ms, z -1.6: the same streaming hitches of 42-49 ms
  in both builds, inside or outside the period counted as the dip). Archived in
  `docs/perf/archive/2026-11-16-amd-resolve-views/`.

Tests: `gfx_render_tests` (a chain of five resolves to one address, each sampled back in the same batch, with a
red/blue swapped one between them: every pixel right, sampled in place, no copy; a 64-row texture over a 40-row
resolve: copied, the rows below zeros), under the validation layer on NVIDIA and AMD.

### What the shaders cost: pipeline statistics, the zero-multiply rule (2026-11-16)

`ANY360_PIPELINE_STATS=<file>` (VK_KHR_pipeline_executable_properties) writes a line a pipeline with what the driver
made of its shaders (`vs <hash>_<modification> + ps <hash>_<modification> | <stage>: numUsedVgprs=..; ...`), and
`ANY360_PIPELINE_ISA=<dir>` their assembly: the executables' internal representations where the driver gives them,
AMD's disassembly through VK_AMD_shader_info. With `ANY360_DRAW_TIMING=1` and `ANY360_DUMP_SHADERS=<dir>` (microcode
and SPIR-V) over a capture in `gfx_replay`, a costly draw's shader can be followed from microcode to the GPU's
instructions.

Crackdown's city on the Radeon iGPU (Windows driver): the costliest shaders use 41-57 VGPRs, no scratch memory, no
dynamically indexed registers; the full-screen shadow pass runs about 3150 instructions in wave64. Much of their
ALU is the Xenos multiply rule (0 times anything, infinity and NaN included, is +0): `min(|a|, |b|) == 0 ? 0 : a * b`
for each multiplied component (ZeroIfAnyOperandIsZero, the mul/mad paths), three instructions more for each
multiply. Sized on the city capture (timing only):

- without the rule: 622 -> 432 ms of GPU time over 29 frames (-30%; the world shader 13.0 -> 6.6 us a draw), but
  0.05% of the pixels go wrong by up to 152 (NaN and infinities through, as in normalizing a zero vector): the rule
  stays;
- contraction allowed (fused multiply-adds): no change;
- the other exact form Mesa recognizes, `(b == 0 ? 0 : a) * (a == 0 ? 0 : b)`: the same images, no measurable gain.

GPUs have this multiply natively (AMD's v_mul_legacy_f32, D3D9's rule), but SPIR-V cannot ask for it. Mesa's
`nir_opt_algebraic.py` turns exactly the translator's pattern into `fmulz` (`(min(abs(a), abs(b)) == 0.0 ? 0.0 :
a * b) -> fmulz(a,b)`), and RADV sets `has_fmulz` for every AMD GPU (`ac_nir.c`), with `ffmaz` from GFX10.3, the
Steam Deck's: on the primary target's driver the rule is one instruction, and the 30% is the Windows driver's. To
confirm on a Steam Deck or another RADV machine (the same capture's per-shader times), not to work around here.
