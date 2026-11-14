# What is in this repository

The pieces of Any360 and how to run its tools. The overview is in the [README](../README.md); the path from static recomp to native codebase in [DESIGN.md](DESIGN.md).

## Contents

| | |
|---|---|
| [docs/DESIGN.md](DESIGN.md) | The path from static recomp to native codebase; what Any360 holds and what a game's repository holds; how a title uses it; counting rules; milestones |
| [db/](../db/) | **The database**: every function of the Xbox 360 platform surface a native port must provide, whatever the title: [system_calls.json](../db/system_calls.json) (2,900 exports of xboxkrnl / xam / xbdm with library, ordinal, area, reference state, Any360 state, the known titles importing each), [instructions.json](../db/instructions.json) (1,842 CPU instruction ids by family), [xdk.json](../db/xdk.json) (statically linked XDK library functions known by name, from [xdk_seed.json](../db/xdk_seed.json)); built by [tools/build_db.py](../tools/build_db.py) |
| [docs/progress.html](progress.html) | Global progress page in the AnyPS5 style: one square per system call, instruction id and XDK function, filterable tables of the database, the known titles as sources, toolkit by stage, history ([progress.json](progress.json), [progress.svg](progress.svg)) |
| [docs/coverage/](coverage/) | Source reports per known title (what feeds the database): [crackdown](coverage/crackdown.md), [simpsons](coverage/simpsons.md), [comparison](coverage/COMPARISON-crackdown-vs-simpsons.md), Crackdown [SDK patch inventory](coverage/patches-crackdown.json) |
| [docs/COMPARISON.md](COMPARISON.md) | What the comparison says: where a second title costs effort (the GPU, twice the functions), where it does not (kernel, CPU) |
| [layer/sys/](../layer/sys/) | **`any360::sys`**, the native system layer (stage 1): portable C++ implementations of the system calls ([include/any360/sys/](../layer/sys/include/any360/sys/)), their PowerPC ABI adapters a recompiled title binds in place of the runtime's ([ppc/exports.cpp](../layer/sys/ppc/exports.cpp)), and two oracles: [sys_io_tests](../layer/sys/tests/sys_io_tests.cpp) runs each stateless call against the reference runtime's implementation on random inputs, [sys_runtime_tests](../layer/sys/tests/sys_runtime_tests.cpp) sets the reference kernel up and runs the thread-bound ones (IRQL, spinlocks, critical sections, TLS...) on a real guest thread, comparing registers, memory, PCR, KTHREAD and KPROCESS (`.\layer\sys\test.ps1`) |
| [layer/gfx/](../layer/gfx/) | **`any360::gfx`**, the GPU side of stage 1: the command processor that executes the title's PM4 (ring buffer kicked through `CP_RB_WPTR`, indirect buffers, register and constant writes, `WAIT_REG_MEM` and the fences the CPU waits on, interrupts, shader microcode loads, bin predication) behind `any360::sys`'s `GpuBackend`, handing draws, resolves and swaps to a graphics-API-agnostic `Renderer`; the Vulkan renderer's first pieces: the Xenos microcode to SPIR-V translator vendored from the ReXGlue SDK (Xenia's, BSD-3, [third_party/xenos_shader](../layer/gfx/third_party/xenos_shader/NOTICE.md)) and a Vulkan device loaded at run time ([docs/GFX.md](GFX.md); tests in [gfx_cp_tests](../layer/gfx/tests/gfx_cp_tests.cpp), [gfx_shader_tests](../layer/gfx/tests/gfx_shader_tests.cpp) and [gfx_vulkan_tests](../layer/gfx/tests/gfx_vulkan_tests.cpp), run by `.\layer\sys\test.ps1`) |
| [docs/SYS.md](SYS.md) | The system layer: layout, the two oracles, what is verified, where the reference stops being an oracle and what comes next |
| [layer/host/rexglue/](../layer/host/rexglue/) | The binding of a title still built on the ReXGlue runtime to Any360's layers, a piece at a time: graphics first (the Vd* system calls and the GPU MMIO to `any360::sys`, `any360::gfx` behind), tested inside the reference runtime by [host_rexglue_tests](../layer/host/rexglue/tests/host_rexglue_tests.cpp) ([docs/GFX.md](GFX.md), step 9) |
| [layer/status.json](../layer/status.json) | Components by stage, where each comes from, its state |
| [docs/TITLE_BUNDLE.md](TITLE_BUNDLE.md) | A ported title's bundle for launchers: one folder per title outside the repositories (manifest, 600x900 cover and 1920x620 banner made by [tools/title_art.py](../tools/title_art.py), and its name, icon and achievements read from the XEX's SPA by [tools/title_metadata.py](../tools/title_metadata.py), the pipeline's `metadata` step), later zipped as a `.any360` file |
| [docs/PIPELINE.md](PIPELINE.md) | The decompile → bind → verify → recompile pipeline, driven by [pipeline/any360.py](../pipeline/any360.py) and one `titles/<title>/pipeline.json` per game |
| [tools/inventory.py](../tools/inventory.py) | Every function of a title with its state (native / verified / library / recompiled) and the functions the port already translated; the per-function dump stays local (`docs/data/`, ignored), the summary feeds the database's library ranges |
| [tools/coverage.py](../tools/coverage.py) | Given a recomp's generated code and an SDK tree: imports (implemented / stub / missing), instruction mnemonics (handled or not), native functions; `--compare` two titles |
| [tools/patch_inventory.py](../tools/patch_inventory.py) | Classifies an SDK patch set by SDK area |
| [tools/build_db.py](../tools/build_db.py) | Rebuilds `db/` from the reference SDK's tables, the title reports and the hand-kept seeds |
| [tools/progress_page.py](../tools/progress_page.py) | Regenerates the progress page from `db/` |

## Running the tools

Standard-library Python 3.11+, run from the repository root. Paths below are the Crackdown and Simpsons checkouts next to this repository.

```bash
python tools/coverage.py --name crackdown --generated ../crackdown-xbox360-recompiled/generated/default --sdk ../crackdown-xbox360-recompiled/out/rexglue-sdk-src --port-src ../crackdown-xbox360-recompiled/src
```

```bash
python tools/coverage.py --name simpsons --generated ../TheSimpsonsGameRewrite/simpsons/generated/default --sdk ../TheSimpsonsGameRewrite/tools/rexglue-sdk --port-src ../TheSimpsonsGameRewrite/simpsons/src
```

```bash
python tools/coverage.py --compare docs/coverage/crackdown.json docs/coverage/simpsons.json
```

```bash
python tools/patch_inventory.py --name crackdown --patches ../crackdown-xbox360-recompiled/patches/rexglue-sdk --purposes ../crackdown-xbox360-recompiled/docs/rexglue-patches.md --game-words crackdown
```

```bash
python tools/inventory.py --name crackdown --generated ../crackdown-xbox360-recompiled/generated/default --port-src ../crackdown-xbox360-recompiled/src --golden ../crackdown-xbox360-recompiled/tests/golden
```

```bash
python tools/progress_page.py
```

The pipeline (Ghidra project, decompile, bind, verify) is described in [docs/PIPELINE.md](PIPELINE.md):

```bash
python pipeline/any360.py crackdown status
```

The system layer builds with the titles' toolchain (Visual Studio's clang++) against a ReXGlue SDK install named by
`reference_sdk` in a `titles/<title>/pipeline.json`, and verifies every system call it provides against that runtime:

```bash
powershell -File layer/sys/test.ps1
```

Before every commit: `python tools/build_db.py` then `python tools/progress_page.py`.

Native replacements of the library functions titles link (`layer/xdk`: the C runtime's `memcpy`, `memmove` and
`memset`, the XDK's vector copy) are Any360's, shared by every title; the titles are where they are checked and
measured. They follow one rule, [docs/NATIVE_FUNCTIONS.md](NATIVE_FUNCTIONS.md): a reference model, unit tests against it, a
benchmark, then the check and the measure in game (`--any360_xdk_verify=true`, `--any360_xdk_bench=true`) before a
function is bound, and optimization only with the tests passing and the benchmark confirming it.

```bash
out/sys/xdk/xdk_crt_tests      # and xdk_xapi_tests: run by layer/sys/test.ps1
out/sys/xdk/xdk_crt_bench      # and xdk_xapi_bench: nanoseconds a call, size by size and on the titles' measured mixes
```

Each title's own repository carries its progress report (a picture, the tables, a roadmap of what is missing and the
state of every function), generated from here and described by `titles/<title>/progress.json`:

```bash
python tools/title_progress.py crackdown     # writes <title repo>/docs/progress and refreshes its README block
```

`--generated` is the ReXGlue codegen output (`*_init.cpp` for the import table, `*_recomp.*.cpp` for the instruction
comments); `--sdk` an SDK source tree. Generated code is never committed here: run the tools against your local tree
and commit the reports.
