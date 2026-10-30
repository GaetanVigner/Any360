# Any360: design

Any360 is the reusable part of turning an Xbox 360 static recompilation into a native PC port. It is to
[ReXGlue](https://github.com/rexglue/rexglue-sdk) what [AnyPS5](https://boykopovar.github.io/AnyPS5/progress.html) is
to a PS5 title: a layer between the recompiled game code and the host that, title after title, replaces emulation with
native implementations, and a progress page that says how much is left for each title.

It was extracted from the [Crackdown port](https://github.com/GaetanVigner/CrackdownRewrite) (ReXGlue v0.10.0, 120
SDK patches, a native Vulkan renderer in progress) and the next title is
[The Simpsons Game](https://github.com/GaetanVigner/TheSimpsonsGameRewrite) (ReXGlue v0.8.0, vendored fork, its own
native renderer plan). This document says what belongs in the layer, what stays with each game, and how a game
consumes the layer.

Rules that do not change: the layer never contains game code or game data. Addresses, hashes, public API names
(kernel exports, XDK function roles), counts and instruction mnemonics are fine; bytes of a title's executable or
assets are not. ReXGlue is BSD 3-Clause and so is this repository ([LICENSE](../LICENSE), [NOTICE.md](../NOTICE.md)).

## 1. What a ReXGlue recomp is made of, and where emulation remains

```text
title's XEX  --rexglue codegen-->  generated/<game>/*.cpp     (native x86-64 code, no CPU emulation)
                                        |  calls __imp__<Export>
                                   rexruntime (kernel: xboxkrnl / xam / xbdm, memory, threads, audio, input, VFS)
                                        |  PM4 ring written by the title's statically linked XDK D3D runtime
                                   rexgpu-xenos (GPU plugin: PM4 parse, register file, EDRAM, RT / texture caches,
                                                 shader translator, Vulkan / D3D12 backends)
```

Three places still emulate, and they are what the coverage tool measures per title ([coverage reports](coverage/)):

| Emulated | Crackdown | The Simpsons Game | How the layer replaces it |
|---|---|---|---|
| Kernel / XAM calls (runtime provides them) | 248 imports, 223 implemented, 25 stubs | 317 imports, 270 implemented, 47 stubs | per-export native implementations, shared |
| CPU instructions (recompiler translates them) | 2.67 M, 340 mnemonics, 100% handled | 2.94 M, 338 mnemonics, 100% handled (1 prose artefact) | codegen patches when a builder is missing or wrong (rounding) |
| GPU (plugin emulates Xenos) | 29 render passes, 1 native (shadow atlas) | not measured yet | draw records + native Vulkan renderer |

The first two are already close to complete for both titles: the kernel stubs left are Xbox Live, voice, UI dialogs
and crypto, and every instruction either game uses has a builder. **The GPU is where the layer's work is**, which is
why the Crackdown port spent its time there and why most of what moves into Any360 is renderer-side.

## 2. The boundary: generic versus game-specific

Taken from the Crackdown repository. "Generic" means another ReXGlue title can use it unchanged or with a rename;
"XDK-bound" means it depends on the XDK version the title was built with (shared by titles of the same era, found by
signature, not by address); "game" means it stays in the title's repository.

### 2.1 SDK patch set (`patches/rexglue-sdk/`, 120 patches) — generic

Every patch applies to the SDK, none to game code. By SDK area ([patches-crackdown.json](coverage/patches-crackdown.json)):

| Area | Patches | In the layer as |
|---|---|---|
| GPU: shared command processor and caches | 31 | performance series: register block copies, cached indirect buffers (0109), draw identity memo (0110), fetch constant caches (0111), EDRAM bookkeeping memo (0112), frame stats (0107) and draw trace (0108, 0113) |
| GPU: Vulkan backend | 26 | the native renderer's host: Vulkan on Windows (0114), submissions and occlusion queries (0038, 0053, 0128), dynamic uniform buffers (0122), timers (0116-0127) |
| GPU: D3D12 backend | 24 | kept for the emulation path on Windows; nothing of the native renderer depends on them |
| kernel: xboxkrnl / XAM | 13 | bug fixes other titles hit (`XamEnumerate` 0060, `RtlCompareStringN` 0087, save-load page recovery 0059/0065, title thread shutdown 0008) |
| core / platform, UI | 12 | waits and clocks instead of spinning (0015-0021, 0026-0028, 0068), logging, ImGui cursors |
| audio | 5 | XMA context fixes (0007, 0050, 0096), callback pacing (0051) |
| input | 3 | keyboard / mouse through `XInputGetKeystroke` (0054), mouse look (0055), activity callback (0094) |
| codegen | 3 | `vpkd3d128` (0005), bit-exact `vrsqrtefp` (0052), single-rounding FMA (0095): helpers emitted into the generated prelude, so a title needs no SDK header change |
| HLE renderer channel | 1 | patch 0129, see 2.2 |
| build | 1 | ThinLTO / PGO of the GPU plugin (0098, 0102) |

15 patches mention the title in their text (commit messages, cvar names such as `crackdown_hle_plugin_channel`); the
code is generic. The patch mechanism itself (numbering, `.linux` / `.windows` suffixes, the rule that a patch must
not move virtual functions or data members of classes the generated code uses, `baseline.env` hashes, the stock-or-
patched stamp) is documented in the Crackdown repository and moves here with the scripts.

### 2.2 Renderer: the HLE channel and the record model — generic, with XDK-bound inputs

The Crackdown renderer plan (`HLE_RENDERER_PLAN.md`, H1-H4) rests on three pieces that know nothing about Crackdown:

- **`HleFrameAbi`** (`src/hle_frame_abi.h`, the same struct in patch 0129's `rex/graphics/hle_frame.h`): what the game
  hands the plugin once a frame through the exported `rex_gpu_hle_submit_frame`: the draw records, the block arena they
  reference, the layout of the render state block (group table), the pass names, per-record shader microcode locations
  and draw packets. Versioned (4), size-checked, copied by the plugin before returning, checked equal on both sides by a
  test. This is the contract of the layer.
- **The draw record model** (`src/hle_records.h`): `RecordKind` (draw, indexed, up vertices, command buffer, clear,
  resolve, GPR flush, buffer draw), `DrawRecord` (pass, args, shaders, offsets of immutable shared state blocks: render
  state, fetch constants, bool/loop, 16 + 16 pages of float constants, index source, microcode) and the frame arena. Records
  reference blocks that consecutive draws share until the title changes them.
- **The PM4 decoder** (`src/pm4_decode.h`, 148 lines, header-only, tested): the Xenos type-0 / type-3 packets the XDK
  D3D runtime and engines emit, reduced to state writes, draws, shader loads, bin (tile) predication and indirect
  buffers. Its opcode allow-list is the XDK's, not the game's.

Two record sources exist, and the layer must accept both, because the two titles chose differently:

| | A: game-side hooks (Crackdown, `src/hle_capture.cpp`) | B: PM4 decoded in the plugin (Simpsons `re/native_renderer_plan.md`) |
|---|---|---|
| Reads | the D3D device's shadow registers and dirty masks at each flushing call, then walks the call's ring output for command buffers | the command stream the XDK already writes, on the GPU command thread |
| Needs from the title | the entry points of the XDK draw / clear / resolve / RunCommandBuffer functions, the device shadow layout | nothing game-specific |
| Gives | per-object identity (call site, pass), the ability to stop emitting PM4 later | one decoder validated by the trace, no transport |
| Status | exact for 99.9% of GPU draws, shadow atlas drawn natively and validated | the vendored SDK has `native_records.cpp` and trace tooling; renderer being converted in place |

Both produce the same `DrawRecord`s. In the layer, the record model and `HleFrameAbi` are the pivot: source A is a
game-side library (`any360::records`, the capture walker generalised), source B is a plugin-side decoder that fills
the same records from the ring; the native pass pipeline (dry run to pipelines, native draw, depth / colour compare
against the emulated target at its resolve, 0129) consumes records and does not care where they came from.

### 2.3 XDK D3D runtime binding — XDK-bound, the key to portability

Every title statically links the XDK's D3D runtime: the draw functions, `RunCommandBuffer`, clears, resolves, the
device shadow with its dirty masks, the ring writer. Crackdown found them by address (209 entry points in
`0x826A0000-0x826D0000`, `docs/progress/d3d_entry_points.txt`; shader object offsets; dirty masks at device +0, +8,
+16, +24, +32). The Simpsons project found the same functions by **signature** against an XDK signature set (174 exact
matches, `re/d3d_method_table.md`). The layer takes the second route: a table of XDK function roles (`Draw`,
`DrawIndexed`, `BeginVertices`, `RunCommandBuffer`, `Clear`, `Resolve`, `SetShaderGPRAllocation`, `IM_LOAD` writer,
...) keyed by a hash of the function's normalised bytes per XDK build, and a tool that, given a title's generated code,
emits the title's `<game>_xdk_bindings.h` (role → `sub_XXXXXXXX`). The table holds hashes and roles, never bytes.
With it, the game-side hooks of source A and the pass tagging become the same code for every title of that XDK era,
and a title with another XDK build adds its hashes, not its hooks.

### 2.4 Tests — generic

- **Guest sandbox** (`tests/guest_sandbox.h`): runs recompiled leaf functions outside the game in a 4 GB guest address
  space whose pages commit on first touch (vectored exception handler on Windows, lazily backed mapping on Linux), with
  the generated prelude; functions that call nothing are extracted from the local generated tree at configure time, and
  golden files keep addresses, inputs and outputs only. Checks a native replacement against the recompiled original,
  and the codegen patches (0052, 0095) against bit-exact expectations.
- **SDK patch guards** (`tests/sdk_patch_guards_tests.cpp`): fail when a rebase loses a patched behaviour.
- **PM4 decode tests**, **per-draw Vulkan tests** (ABI equality), **frame-trace comparison** (`tools/hle-trace-compare.ps1`,
  records vs GPU trace).

### 2.5 Tools — generic

Coverage (this repository, from `tools/progress_map.py`), crash mapping (host address → guest function and
instruction; the XEX decrypt and mapping recipe), missing-function discovery (`tools/find-missing-functions.py`; the
Simpsons project solved the same problem with an `LD_PRELOAD` trap library and a hand-kept `functions_patch.toml`),
the progress chart, the build scripts that produce a patched `rexruntime` / `rexgpu-xenos` from the baseline.

### 2.6 What stays with the game

Render pass hooks and names (Crackdown: 29 passes, tagged by hooking the pass functions), the pass chosen for each
native step, settings, launcher, achievements, cheats, save compatibility, title updates, frame pacing and physics
step fixes (`vehicle_action_step`, `character_step_fixes`), language and content handling, and the game manifest. In
Any360 they appear only as counts on the progress page.

## 3. How a title consumes the layer

Three ways were considered.

| | A. Pinned SDK + patch series (Crackdown today) | B. Fork of ReXGlue (Simpsons today: vendored v0.8.0, 8k-line modified Vulkan backend) | C. Upstream into ReXGlue |
|---|---|---|---|
| New SDK release | re-apply the series; conflicts name exactly what changed | rebase a fork; conflicts spread across files | nothing to do |
| Two titles sharing | one series, one baseline, each title picks its options | two forks diverge at once (0.8.0 vs 0.10.0 already) | trivial |
| Experimental renderer work | fine: a patch per step, each measured | fine | too early: the ABI changes weekly |
| Cost of a change | small and reviewable | low friction, high drift | review latency |

**Recommendation: A now, C as each piece settles.** Any360 ships `sdk/baseline.env` (version, tag, commit, archive
hashes) and `sdk/patches/<area>/NNNN-name.patch`, the scripts that apply them and build the runtime and the GPU
plugin, and a CMake package:

```cmake
# in the title's CMakeLists.txt
include(FetchContent)
FetchContent_Declare(any360 GIT_REPOSITORY https://github.com/<owner>/any360 GIT_TAG v0.1.0)
FetchContent_MakeAvailable(any360)
any360_setup_target(simpsons           # the generated code target
    XDK_BINDINGS generated/default      # where the binding tool finds the title's functions
    FEATURES hle_records pm4_decode sandbox_tests)
```

`any360_setup_target` fetches the pinned SDK, applies the series (all of it: a title does not pick patches, it picks
cvars at run time, as Crackdown does with `gpu_backend`, `crackdown_hle_records`...), builds the patched runtime, links
the game-side library, generates the XDK bindings header, and registers the tests. A title keeps its own `patches/`
directory for what is not generic yet; the layer's series applies first, the title's after. The Simpsons recomp would
first move from its vendored 0.8.0 to the pinned 0.10.0 plus the series, and bring its renderer work as patches: the
coverage comparison says nothing in its instruction or import profile stands in the way (section 5).

For C, the ordering is by stability: kernel fixes and threading patches first (already generic, low risk), then the
GPU shared-layer performance series with their cvars off by default, then the HLE channel once the ABI stops changing
(it went from version 1 to 4 in a week), the native renderer last.

## 4. Measuring what each title needs

### 4.1 Statically: `tools/coverage.py`

Inputs: a recomp's generated code (the `*_init.cpp` import table and the `// mnemonic` comment the codegen writes
before each instruction) and an SDK source tree (the kernel `REX_EXPORT*` / `REX_HOOK_RAW(__imp__*)` definitions,
`export_table.inc`, the disassembler's opcode table and the recompiler's dispatch table). Outputs per title: imports by
library and state, stubs and missing exports (the list a native port writes first), `Vd*` imports, instruction
families, mnemonics without a builder, `REX_UNIMPLEMENTED` markers, recompiled functions and how many the port
replaces natively. `--compare` writes what only one title needs. `--compare-sdk` shows the same imports under another
SDK tree (used here to check that the Simpsons fork's 0.8.0 kernel and stock 0.10.0 provide the same exports: they do).

### 4.2 At run time: the GPU census (planned)

Which Xenos features a title uses is not in its code: PM4 opcodes, shader instruction families (Xenos microcode:
fetch, ALU, export, memexport), render target and texture formats, MSAA counts, EDRAM aliasing, predicated tiling,
occlusion queries, `WAIT_REG_MEM` patterns. Crackdown's patches 0107 (`gpu_frame_stats`) and 0108/0113 (draw trace
with resolve data) already collect most of it. The census tool reads a trace and writes `docs/coverage/gpu-<title>.json`
in the same shape as the static report, and the progress page gains a "GPU features" table with one column per title,
each feature marked emulated / native. This is the AnyPS5 "GPU shader instructions" table for Xenos.

### 4.3 Counting rules

- *Provided* import: `REX_EXPORT` or `REX_HOOK_RAW(__imp__X)` in the SDK kernel, or a mapped kernel variable. A
  `REX_EXPORT_STUB*` counts as a stub even when the title never notices.
- *Handled* instruction: the opcode id the disassembler assigns to the mnemonic has a builder in
  `instruction_dispatch.cpp`. Extended mnemonics and macros (`slwi`, `clrlwi`) count under their base opcode. Branch
  hint suffixes (`beq+`) are stripped. Counts are static (one per emitted instruction), not dynamic.
- *Native* function: a `REX_HOOK`, `REX_HOOK_RAW`, `REX_FUNC` or `*_BIND*` of a `sub_XXXXXXXX` in the port's sources.
- *Native* render pass: validated equal to the emulated image at its resolve, then shown from the native renderer.
- *Layer component done*: in this repository, buildable and used by at least one title from here, not from a copy.

## 5. Crackdown → The Simpsons Game: what the comparison says

Full tables: [COMPARISON-crackdown-vs-simpsons.md](coverage/COMPARISON-crackdown-vs-simpsons.md),
narrative: [COMPARISON.md](COMPARISON.md).

- **Kernel**: 225 imports in common. The 92 only Simpsons uses are mostly implemented already: the 14 `XMA*` kernel
  context calls (the Simpsons engine drives the XMA decoder itself, Crackdown goes through XAudio: the XMA patches
  0007/0050/0096 matter there first), `XamContent*` (DLC and storage enumeration), `ExAllocatePool*`, APCs and scatter
  reads. Its 47 stubs are Xbox Live (`NetDll_*`, `XNetLogon*`), the dashboard UI dialogs (13 `XamShow*UI`) and
  `VdQuerySystemCommandBuffer` / `VdSetSystemCommandBuffer`: the one GPU-side stub, worth checking in the plugin.
  Crackdown's 23 own imports are `XamTask*`, STFS devices and the wide-string CRT.
- **CPU**: the same 326 mnemonics plus 15 only Crackdown uses and 12 only Simpsons uses, all with builders. The Simpsons
  extras are saturating integer VMX (`vaddshs`, `vaddsws`, `vsubsws`, `vpkshus`, `vpkuwus`, `lvewx`), the kind of code a
  software audio mixer or animation decompressor compiles to; worth a sandbox golden test of the saturation builders
  before trusting them, as was done for `vrsqrtefp` and the FMA forms.
- **Size**: 81,874 recompiled functions against 41,219, in 90 translation units against 177 (a different partitioning
  of the same codegen).
- **GPU**: unknown until the census runs on the Simpsons build. Its plan says the XDK writes every draw's state into
  the command stream and that shader literals bypass the device shadow (`LOAD_ALU_CONSTANT`), both of which Crackdown
  measured too (M1/M2): the two renderers agree on the facts and differ on where the records are built.

## 6. Repository layout (target)

```text
any360/
  README.md  LICENSE  NOTICE.md
  docs/            DESIGN.md (this), COMPARISON.md, progress.html / .svg / .json, coverage/<title>.{md,json}
  layer/           status.json (components and states)
  sdk/             baseline.env, patches/<area>/NNNN-*.patch, apply + build scripts        (planned)
  src/any360/      hle_frame_abi.h, hle_records.h, pm4_decode.h, records walker, XDK bindings (planned)
  tests/           guest_sandbox.h, pm4 decode tests, ABI equality test, patch guards      (planned)
  tools/           coverage.py, patch_inventory.py, progress_page.py; census, crash map, missing functions (planned)
  cmake/           any360_setup_target                                                      (planned)
```

## 7. First milestones

| # | Milestone | Done when |
|---|---|---|
| L1 | Skeleton, design, coverage of both titles, progress page | this commit |
| L2 | SDK series moved: `sdk/baseline.env`, patches by area, build scripts; Crackdown builds from the layer's copy | Crackdown CI green with `patches/rexglue-sdk/` reduced to its game-only patches (none expected) |
| L3 | Game-side library: `hle_frame_abi.h`, `hle_records.h`, `pm4_decode.h`, the record walker with its XDK roles abstracted | Crackdown's capture uses `any360::records`; the ABI test passes from the layer's header |
| L4 | XDK binding tool and table for the Crackdown and Simpsons XDK builds | both titles' D3D entry points resolved by hash, `d3d_entry_points.txt` regenerated from it |
| L5 | Simpsons on the pinned SDK + series, records flowing (source B or A), census run, GPU table on the progress page | the Simpsons repository consumes Any360 and its first pass is compared against emulation |
