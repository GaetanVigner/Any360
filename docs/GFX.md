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
   AMD. Next: depth resolves, more destination formats, MSAA, 3D / array destinations, and host-to-host copies so
   a resolved texture need not go through guest memory; then a frame can be presented.
7. **Present** (first step done, 2026-11-01): `XE_SWAP` presents the texture of fetch constant 0, as `VdSwap` (now
   native in any360::sys) writes it: the front buffer is read from guest memory as RGBA8 with its swizzle (the
   renderer's `frame()`, checked headless in `gfx_render_tests`), then the `Presenter` shows it in a window. The
   window comes from SDL3 and the platform layer makes the Vulkan surface from its native handle (Win32 for now,
   X11 / Wayland next), which also works with SDL builds that lack SDL's own Vulkan support (the reference SDK's).
   `gfx_window_demo` runs the whole chain at 77 FPS (RTX 5070 Ti) and 61 FPS (Radeon integrated) for a 1280x720
   frame; the time goes into the deliberately simple paths (all mirrored memory copied per draw, one submission
   per draw, CPU untiling and readback). Learned: readback buffers need cached host memory (from 3 to 77 FPS).
8. **Records**: the Crackdown record model, so a title bound at the D3D level skips PM4 entirely.
9. **Binding**: the title's MMIO stores to 0x7FC8xxxx go to `GraphicsKernel::WriteRegister`; a title's frames
   then reach the window.

It avoids geometry shaders and rasterizer-ordered views so that it runs through MoltenVK and on mobile GPUs:
rectangle and quad lists are expanded in the vertex stage.
