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
   32x32 textures. `getBCF` (the border colour fraction) is not implemented by the translator: it stays to do.
   Formats and primitives (2026-11-05): every vertex format through a vertex shader's fetch into an interpolator
   (unsigned, signed and integer forms, an exponent adjustment; components a format lacks read 0, a title's
   shader asking for 1 through the swizzle), every colour render target format but k_8_8_8_8 read back as the host
   stores it, D24FS8 depth, and triangle fans, polygons (a fan), quad strips (a triangle strip), line lists,
   strips and loops (a strip back to the first vertex, expanded like quad lists). The command processor now
   gathers an immediate shader load that wraps around the ring's end (it assumed it never would; the bench's
   ring did).
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
