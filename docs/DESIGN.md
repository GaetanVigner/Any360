# Any360: design

Any360 is a toolkit for turning an Xbox 360 game into a **portable native codebase**: a decompilation project in the
sense of [awesome-decomp-games](https://submitgame.github.io/awesome-decomp-games/), that builds for Windows, Linux,
macOS, Android, x86-64, ARM64 and, in the end, WebAssembly, from reconstructed source and the player's own copy of the
game's data. It is to Xbox 360 titles what [AnyPS5](https://boykopovar.github.io/AnyPS5/progress.html) is to PS5
titles: the system calls, GPU and CPU semantics a game needs, implemented once and reused title after title, with a
progress page that says how much of each title is native.

[ReXGlue](https://github.com/rexglue/rexglue-sdk) is the **starting point, not the destination**. A ReXGlue static
recompilation gives a running game on day one: that running game is Any360's scaffold and its oracle, the thing every
native replacement is checked against. The goal is a game that no longer links the ReXGlue runtime, the GPU emulation
or the generated PowerPC-context code at all.

Started from the [Crackdown port](https://github.com/GaetanVigner/CrackdownRewrite) (ReXGlue v0.10.0, 120 SDK
patches, a native Vulkan renderer in progress, 103 functions already replaced by hand-written C++); the next title is
[The Simpsons Game](https://github.com/GaetanVigner/TheSimpsonsGameRewrite). This document says what the path is,
what Any360 holds, what each game's repository holds, and how progress is counted.

Rules that do not change: Any360 holds no game data (no executables, assets, ISOs, extracted files) and no recompiled
output. The reconstructed source of a title lives in that title's own repository, as decompilation projects do; Any360
holds the toolkit, the system layer, the renderer, and reports made of counts, hashes, addresses and public API names.
ReXGlue is BSD 3-Clause and so is Any360 ([LICENSE](../LICENSE), [NOTICE.md](../NOTICE.md)).

## 1. The path: from static recomp to native codebase

```text
stage 0  static recomp (ReXGlue)     generated PPC-context C++ + rexruntime (kernel) + rexgpu-xenos (Xenos emulation)
            |  the oracle: behaviour to match, functions to test against, coverage of what the game needs
stage 1  native system layer         any360::sys replaces the kernel / XAM imports; any360::gfx replaces the GPU
            |  emulation (draw records -> Vulkan); the generated code still runs, nothing of ReXGlue's runtime does
stage 2  decompilation               function by function, sub_XXXXXXXX -> readable C++ with recovered types, each
            |  checked against the recompiled original in the sandbox; engine libraries recognised and replaced
stage 3  portable native codebase    no PPC context, no 4 GB guest memory, data converted at install time; builds for
                                     every target the platform layer supports, WebAssembly included
```

Each stage is measurable per title, and the stages overlap: Crackdown is at stage 1 for the GPU (one render pass
native, 29 to go), at stage 2 for 103 functions, and at stage 0 for everything else.

### Stage 0: the static recomp as scaffold and oracle

ReXGlue translates the XEX into C++ that manipulates a PowerPC register context and a big-endian guest memory, and
links a runtime that emulates the kernel (`xboxkrnl`, `xam`) and a plugin that emulates the Xenos GPU from the PM4
stream the title's statically linked XDK D3D runtime writes. Everything the game does is visible from here:

- **what it needs**: its kernel / XAM imports and the instructions its code uses ([tools/coverage.py](../tools/coverage.py),
  reports in [coverage/](coverage/)); its GPU features at run time (census, 4.2);
- **what it does**: every function can be run in isolation in the guest sandbox against recorded inputs and outputs,
  which is how a decompiled or hand-written replacement is proven equal before it replaces the original;
- **where the time goes**: the Crackdown profiler, frame stats and draw traces found that the GPU emulation, not the
  recompiled code, is the limit (a 2007 game on four Zen 2 cores spends its frame parsing PM4 and emulating EDRAM).

The Crackdown SDK patch series (120 patches) belongs to this stage: it keeps the scaffold fast and correct while the
later stages are built on it, and it is where ReXGlue upstream can take fixes back. It is not the product.

### Stage 1: a native system layer, `any360::sys` and `any360::gfx`

The 248 (Crackdown) and 317 (Simpsons) imports are the whole surface between a title and the console. Stage 1
implements them once, portably, in Any360:

| Area | Imports (both titles) | Native implementation |
|---|---|---|
| threads, synchronisation, APCs, timers (`Ke*`, `Nt*Event`, `Rtl*CriticalSection`) | ~60 | a scheduler over host threads with Xbox semantics (priorities, affinity, alertable waits), the clock at 50 MHz time base |
| memory (`MmAllocatePhysicalMemory`, `NtAllocateVirtualMemory`, `ExAllocatePool`) | ~15 | guest address space manager (stage 1-2), then plain allocators (stage 3) |
| files and storage (`NtCreateFile`, `NtReadFile*`, `IoCompleteRequest`, STFS, `XamContent*`) | ~40 | virtual file system over the installed game data, saves and DLC in a user folder, STFS/ XContent packages read natively |
| audio (`XAudio*`, `XMA*`) | ~20 | XMA decoder (from FFmpeg's `wmapro`/`xma2`) feeding a platform mixer (SDL3 audio) |
| input (`XamInput*`, `XInputGetKeystroke`) | ~8 | SDL3 gamepads, keyboard and mouse mapping |
| video (`Vd*`) | ~15 | the renderer's ring: swaps, fences, `WAIT_REG_MEM` honoured by `any360::gfx` |
| user, profile, achievements, UI (`XamUser*`, `XamShow*UI`, `XamEnumerate`) | ~45 | local profile store, offline achievements, host dialogs |
| Xbox Live, voice (`NetDll_*`, `XNet*`, `XamVoice*`) | ~45 | offline answers that make the title take its offline path (never implemented as network) |
| CRT and strings (`sprintf`, `Rtl*String`, `XeCrypt*`) | ~25 | the host CRT, SHA/HMAC from a small crypto library |

The coverage reports list each import with its state; stage 1 is done for a title when its import table resolves
to `any360::sys` and the binary no longer links `rexruntime`.

The GPU is the larger half. The title's XDK D3D runtime writes PM4; Any360 turns the title's draws into **draw
records** (`DrawRecord`: pass, shaders, immutable shared state blocks, index source) and renders them natively with
Vulkan: host render targets per surface instead of EDRAM emulation, one pass instead of per-tile replays, the Xenos
microcode translated to SPIR-V once and cached, textures untiled at load. Two record sources exist and both are kept:
game-side hooks on the XDK draw functions (Crackdown: exact for 99.9% of GPU draws, the shadow atlas drawn natively and
validated), or a decode of the PM4 stream (the Simpsons plan). At stage 3 the XDK D3D runtime itself is decompiled and
its calls become `any360::gfx` calls directly; the records are then the renderer's internal format, no PM4 anywhere.

### Stage 2: decompilation, function by function

The reconstructed source replaces the generated code one function at a time, through the same hook mechanism the
Crackdown port uses today (`CRACKDOWN_BIND(address, handler)`: the generated `sub_X` jumps to the C++ handler, which
can still call the original). The workflow, run with REA (reverse-engineer-anything) on the decrypted XEX image:

1. **Structure first**: functions (ReXGlue's analysis plus `find-missing-functions.py` for pointer-only targets),
   vtables and RTTI class names (the Crackdown image has them: the engine's classes, Havok, XAudio, the XDK), the
   call graph, string references, the data layout. Output: a symbol map (address → name, class, module) kept in the
   title's repository as data, never bytes.
2. **Recognise libraries**: the XDK D3D runtime (209 functions in Crackdown, matched by signature in the Simpsons
   project), Havok, XAudio2, the CRT, zlib and friends. They are not decompiled: they are replaced by Any360's
   implementation (D3D → `any360::gfx`) or by the real library (zlib) or by a decompiled shared copy (Havok 4.x once,
   reused by every title that ships it).
3. **Decompile the game's own code** module by module (Ghidra's PowerPC decompiler, cleaned up with the recovered
   types), bind each function, prove it in the sandbox against the recompiled original on recorded inputs, and keep
   the golden file. The guest sandbox already runs leaf functions in a lazily committed 4 GB guest space; stage 2 grows
   it to functions that call others (the callees still recompiled) and to recorded memory snapshots.
4. **Count**: functions decompiled and verified / total, per module (progress page).

This is what the 103 Crackdown bindings already are, done by hand for frame pacing, physics step fixes, audio and the
renderer hooks; stage 2 makes it systematic and tool-driven.

### Stage 3: the portable codebase

When the generated code is gone, so are the PowerPC context and the guest memory, and with them the two things that
keep a recomp on x86-64: the 4 GB reservation and the VMX-to-SSE intrinsics (SIMDe already covers ARM64; WebAssembly
cannot reserve 4 GB nor fault pages in). Big-endian asset data is converted once at install (the launcher already
extracts the ISO and installs updates). The platform layer is small: SDL3 for windows, input and audio; Vulkan with a
WebGPU or Metal (via MoltenVK) backend for the renderer; the CRT. Targets: Windows, Linux (Steam Deck first), macOS,
Android, WebAssembly.

## 2. What Any360 holds, what a game's repository holds

| | Any360 (this repository) | A title's repository (CrackdownRewrite, TheSimpsonsGameRewrite) |
|---|---|---|
| stage 0 | the pinned ReXGlue baseline and SDK patch series; the build scripts; the coverage and census tools | the game manifest, the generated code (local, never committed), the title's own patches |
| stage 1 | `any360::sys` (portable kernel / XAM semantics), `any360::gfx` (records, PM4 decoder, native renderer), the frame ABI, platform layer | the import binding table, render pass names and hooks, settings, launcher, achievements data |
| stage 2 | the symbol-recovery and decompile pipeline (REA scripts, sandbox, golden format, binding macros), shared decompiled libraries (Havok, XDK D3D) | the title's symbol map, its reconstructed source and goldens |
| stage 3 | the platform layer, asset converters' framework | the title's asset converters and build targets |
| always | progress page, per-title reports (counts, names, addresses, hashes) | the game-specific code |

What moves from the Crackdown repository first, because it is already generic: the SDK patch series, `hle_frame_abi.h`,
`hle_records.h`, `pm4_decode.h`, the record walker with its XDK roles abstracted, `guest_sandbox.h` and the golden
format, the patch guard tests, `find-missing-functions.py`, the crash mapping recipe. The XDK D3D runtime is bound by
**signature** (a hash of the normalised function bytes → role), as the Simpsons project does, so the renderer hooks and
the stage 2 library recognition are the same code for every title built with the same XDK; the table holds hashes and
roles, never bytes.

## 3. How a title uses Any360

A title's repository depends on Any360 as a CMake package (FetchContent or submodule, pinned to a tag) and declares
what it is at:

```cmake
include(FetchContent)
FetchContent_Declare(any360 GIT_REPOSITORY https://github.com/GaetanVigner/Any360 GIT_TAG v0.1.0)
FetchContent_MakeAvailable(any360)
any360_title(crackdown
    MANIFEST crackdown_manifest.toml          # stage 0: ReXGlue codegen, pinned SDK + Any360 series
    XDK_BINDINGS generated/default            # signature-resolved XDK roles for this build of the game
    SYSTEM any360::sys                        # stage 1: native system layer instead of rexruntime
    GRAPHICS any360::gfx                      # stage 1: native renderer instead of rexgpu-xenos
    DECOMP src/decomp                         # stage 2: reconstructed source bound over the generated code
    GOLDENS tests/golden)
```

Each option can stay on the ReXGlue side while the native one is built (`SYSTEM rexruntime`, `GRAPHICS rexgpu-xenos`)
so that A/B comparisons against the oracle remain possible until a title reaches stage 3. The Simpsons recomp would
first move from its vendored v0.8.0 fork to the pinned baseline plus the series (its kernel export set is the same,
nothing in its instruction or import profile differs in a way that matters, section 5), then take `any360::gfx` with
PM4 decode as its record source.

## 4. Measuring

### 4.1 Statically: `tools/coverage.py`

Inputs: a recomp's generated code (import table and the one `// mnemonic` comment per instruction) and an SDK tree.
Outputs per title: imports by library and state (implemented / stub / variable / missing in the runtime), the stubs a
native port must handle first, the `Vd*` imports, instruction families, mnemonics without a recompiler builder,
functions and how many the port replaces natively. `--compare` writes what only one title needs. At stage 1 the same
tool reports the state against `any360::sys` instead of the ReXGlue runtime.

### 4.2 At run time: the GPU census (planned)

PM4 opcodes, shader instruction families, render target and texture formats, MSAA, EDRAM aliasing, predicated tiling,
occlusion queries, memexport: collected by the stage 0 scaffold (Crackdown patches 0107 `gpu_frame_stats`, 0108/0113
draw trace) into `coverage/gpu-<title>.json`, shown on the progress page as emulated / native per feature.

### 4.3 Counting rules

The unit of progress is the platform surface, not a title: `db/` lists every function a native port of any Xbox 360
game must provide (the exports of xboxkrnl / xam / xbdm, the CPU instruction ids, the statically linked XDK library
functions known by name) and the progress page counts them globally. A title contributes its imports and its
instruction mix (so that what matters first comes first) and tracks its own game-specific progress in its own repository.

- *Native* entry: implemented in this repository (`any360::sys`, `any360::gfx`, the decompilation rules;
  `layer/sys/native.json` until the sources carry the names) and verified against the reference. *Ported*: implemented
  natively in a known title's repository (`__imp__X` provided by the port), to be moved here. *Reference*: runs today only
  through ReXGlue's implementation. *Stub* / *missing*: runs nowhere. The headline percentage counts native and ported.
- *Provided* import: `REX_EXPORT` or `REX_HOOK_RAW(__imp__X)` in the runtime, or a mapped kernel variable; a stub counts
  as a stub even when the title never notices. *Native* import: implemented in `any360::sys`.
- *Handled* instruction: the opcode id the disassembler assigns to the mnemonic has a builder; extended mnemonics and
  macros count under their base opcode; branch hints are stripped; counts are static.
- *Native* function: bound to reconstructed or hand-written C++ (`REX_HOOK`, `REX_FUNC`, `*_BIND*`). *Verified*: a
  golden test proves it equal to the recompiled original. Stage 2 progress is verified native functions / total.
- *Native* render pass: validated against the emulated image at its resolve, then shown from the native renderer.
- *Component done*: in this repository, buildable, used by at least one title from here rather than from a copy.

## 5. Crackdown → The Simpsons Game: what the comparison says

Full tables: [COMPARISON-crackdown-vs-simpsons.md](coverage/COMPARISON-crackdown-vs-simpsons.md), narrative:
[COMPARISON.md](COMPARISON.md).

- **Kernel**: 225 imports in common. The 92 only Simpsons uses are mostly implemented in ReXGlue already: 14 `XMA*`
  kernel context calls (its engine drives the XMA decoder itself; Crackdown goes through XAudio), `XamContent*`,
  `ExAllocatePool*`, APCs, scatter reads. Its 47 stubs are Xbox Live, 13 dashboard dialogs and
  `VdQuery/SetSystemCommandBuffer`, the one GPU-side stub. For `any360::sys` this means: XMA decoding and content
  packages are stage 1 work on the Simpsons side first; the offline-path answers are shared.
- **CPU**: 326 mnemonics in common, 15 only Crackdown, 12 only Simpsons (saturating integer VMX: `vaddshs`, `vaddsws`,
  `vsubsws`, `vpkshus`, `vpkuwus`, `lvewx`), all with builders. Stage 2 decompiles that code into plain C++ anyway;
  until then, sandbox goldens for the saturating builders.
- **Size**: 81,874 functions against 41,219: twice the decompilation work, the same libraries to recognise (both 2007
  titles: XDK of the same era, Havok in Crackdown, EA's own engine in the Simpsons).
- **GPU**: unmeasured until the census runs on the Simpsons build; its documentation of the command stream (memory
  records after the packet that reads them, tiled resolve destinations, 20e4 depth) matches what Crackdown measured.

## 6. Repository layout (target)

```text
any360/
  README.md  LICENSE  NOTICE.md
  docs/            DESIGN.md, COMPARISON.md, progress.html / .svg / .json, coverage/<title>.{md,json}
  layer/           status.json (components by stage, with states)
  sdk/             stage 0: baseline.env, patches/<area>/NNNN-*.patch, apply + build scripts            (planned)
  src/sys/         stage 1: any360::sys, the portable kernel / XAM semantics                            (planned)
  src/gfx/         stage 1: hle_frame_abi.h, hle_records.h, pm4_decode.h, record walker, Vulkan renderer (planned)
  src/platform/    stage 3: SDL3 window / input / audio, file system, build targets                      (planned)
  decomp/          stage 2: REA pipeline (symbol recovery, library signatures, decompile + bind + golden) (planned)
  tests/           guest_sandbox.h, pm4 decode tests, ABI equality test, patch guards                   (planned)
  tools/           coverage.py, patch_inventory.py, progress_page.py; census, crash map, missing functions
  cmake/           any360_title                                                                         (planned)
```

## 7. First milestones

| # | Milestone | Done when |
|---|---|---|
| L1 | Skeleton, design, coverage of both titles, progress page | done (2026-10-30) |
| L2 | Stage 0 moved: baseline, patch series by area, build scripts; Crackdown builds from Any360's copy | Crackdown's `patches/rexglue-sdk/` reduced to game-only patches (none expected) |
| L3 | Stage 1 graphics: `hle_frame_abi.h`, `hle_records.h`, `pm4_decode.h`, the record walker with XDK roles abstracted, the native pass pipeline, in `src/gfx` | Crackdown's capture and native shadow atlas run from `any360::gfx` |
| L4 | XDK binding by signature for the Crackdown and Simpsons builds | both titles' D3D entry points resolved by hash; Simpsons records flowing |
| L5 | Stage 1 system: `any360::sys` for the ~60 threading and ~40 file imports first, A/B against rexruntime | Crackdown boots to the city with those imports native |
| L6 | Stage 2 pipeline: REA symbol recovery and library recognition on both images, sandbox for non-leaf functions, first module decompiled and verified | progress page shows verified functions per module for both titles |
