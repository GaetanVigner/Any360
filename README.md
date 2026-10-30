# Any360

**A toolkit for turning Xbox 360 games into portable native codebases.** Start from a static recompilation
([ReXGlue](https://github.com/rexglue/rexglue-sdk)), which gives a running game on day one and serves as the oracle;
replace its emulated system calls and GPU with native, portable implementations; decompile the game's own code
function by function, each proven against the original; end with a codebase that builds for Windows, Linux, macOS,
Android, x86-64, ARM64 and WebAssembly from reconstructed source and the player's own copy of the game data, the way
the projects on [awesome-decomp-games](https://submitgame.github.io/awesome-decomp-games/) do. What one title needed
is reused by the next; a progress page says how native each title is.

Started from the [Crackdown port](https://github.com/GaetanVigner/CrackdownRewrite) (ReXGlue v0.10.0, 120 SDK patches,
native Vulkan renderer in progress, 103 functions already rewritten). Next title:
[The Simpsons Game](https://github.com/GaetanVigner/TheSimpsonsGameRewrite).

**No game data, no game binaries.** Any360 holds the toolkit, the system layer, the renderer and reports made of counts,
hashes, addresses and public API names. A title's reconstructed source lives in that title's own repository. Bring
your own copy of the game.

## The path

| Stage | What runs | What Any360 provides |
|---|---|---|
| 0 | ReXGlue static recomp: generated PowerPC-context C++, emulated kernel and GPU | pinned SDK + patch series, coverage and census tools, the oracle for everything below |
| 1 | the same generated code over `any360::sys` (native system calls) and `any360::gfx` (draw records → Vulkan) | the portable system layer and the native renderer; nothing of ReXGlue's runtime left |
| 2 | reconstructed C++ bound over the generated functions, verified in the sandbox | REA pipeline: symbol recovery, library recognition, decompile + bind + golden |
| 3 | a native codebase, no PPC context, no guest memory | platform layer (SDL3, Vulkan / Metal / WebGPU), asset conversion at install |

Details, boundaries and milestones: [docs/DESIGN.md](docs/DESIGN.md).

## What is here

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | The path from static recomp to native codebase; what Any360 holds and what a game's repository holds; how a title uses it; counting rules; milestones |
| [docs/progress.html](docs/progress.html) | Progress page: system calls, instructions, native functions, components by stage ([progress.json](docs/progress.json), [progress.svg](docs/progress.svg)) |
| [docs/coverage/](docs/coverage/) | Per-title coverage: [crackdown](docs/coverage/crackdown.md), [simpsons](docs/coverage/simpsons.md), [comparison](docs/coverage/COMPARISON-crackdown-vs-simpsons.md), Crackdown [SDK patch inventory](docs/coverage/patches-crackdown.json) |
| [docs/COMPARISON.md](docs/COMPARISON.md) | What the comparison says: where a second title costs effort (the GPU, twice the functions), where it does not (kernel, CPU) |
| [layer/status.json](layer/status.json) | Components by stage, where each comes from, its state |
| [tools/coverage.py](tools/coverage.py) | Given a recomp's generated code and an SDK tree: imports (implemented / stub / missing), instruction mnemonics (handled or not), native functions; `--compare` two titles |
| [tools/patch_inventory.py](tools/patch_inventory.py) | Classifies an SDK patch set by SDK area |
| [tools/progress_page.py](tools/progress_page.py) | Regenerates the progress page |

![Any360 progress](docs/progress.svg)

## Running the tools

Standard-library Python 3.11+. Paths below are the Crackdown and Simpsons checkouts next to this repository.

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
python tools/progress_page.py
```

`--generated` is the ReXGlue codegen output (`*_init.cpp` for the import table, `*_recomp.*.cpp` for the instruction
comments); `--sdk` an SDK source tree. Generated code is never committed here: run the tools against your local tree
and commit the reports.

## State

Milestone L1: skeleton, design, coverage of both titles, progress page. Crackdown is at stage 1 for one render pass
(the shadow atlas, drawn natively and validated) and at stage 2 for 103 functions; everything else is stage 0. The
layer's code still lives in the Crackdown repository; [layer/status.json](layer/status.json) lists each piece and
[docs/DESIGN.md](docs/DESIGN.md#7-first-milestones) the order it moves in.

## License

BSD 3-Clause ([LICENSE](LICENSE)). ReXGlue, which stage 0 patches and links, is BSD 3-Clause too; see
[NOTICE.md](NOTICE.md) for what the repository may and may not contain.
