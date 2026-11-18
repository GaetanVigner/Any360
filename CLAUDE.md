# Any360: working rules

Rules the owner set for every session working in this repository. A new session follows them as written; when the
owner adds a rule, it goes here (or into a skill under `.claude/skills/`) in the same change.

## Language

- Talk to the owner in French. Code, comments, docs and commit messages in English.

## What Any360 is

- A generic toolkit for rewriting Xbox 360 titles. The games (Crackdown, The Simpsons Game, Test Drive Unlimited,
  Call of Duty: Black Ops) are test benches, not targets: native functions are written for every title, never tuned
  to or presented around one title, and a title never keeps its own copy beside the shared one.
- Generic first. Per-title code only where a feature cannot work without it (a title adapter), and title-specific
  extras (preloads, remaster work) only in a later fine-tuning phase.
- The goal is every title on `any360::sys` and native code, with ReXGlue only as an oracle: keep hybrid glue thin and
  clearly temporary.
- Current graphics direction (docs/D3D.md): replace each title's XDK D3D runtime with native code that writes no PM4,
  Crackdown first, toward a native Vulkan renderer whose primary target is small hardware (Steam Deck class).

## Doing the work

- A feature is done right or left out (switched off): no half version that still costs and gives little.
- Every native function follows `docs/NATIVE_FUNCTIONS.md` and the `any360-native-function` skill: a reference model,
  unit tests against it, a benchmark, the in-game check (0 different) and the in-game measure before it is bound;
  optimizations are kept only when the benchmark confirms them and the tests still pass.
- Every native must be at least as fast as the title's original (1x or better) and allocate as little as possible.
- Every block checked in game gets an automatic unit test registered with the suite (behaviour, memory, CPU/GPU,
  leaks) in the same change.
- Every function ported to Any360 (a native of a title's code, a system call of `any360::sys`, a D3D or library
  function), in the same change: unit tests of its outputs (every byte it writes and every value it returns, against
  a reference: the original, a standard's test vectors, or the documented behaviour) registered with the suite, and a
  benchmark (time a call and host memory a call, bytes and allocations, against the original when there is one and
  against what the host already offers when it does the same work), with its results saved in the repository under
  `docs/perf/archive/<date>-<step>/` and listed in `docs/perf/README.md`. A function without both is not done.
- Prefer pooled and reused buffers to allocations: RAM is limited on the target devices.
- Register new natives in the database (`db/xdk_seed.json`, `layer/sys/native.json`, `titles/<title>/d3d_names.json`,
  one entry a line), then run `python -I tools/build_db.py` and `python -I tools/progress_page.py`.

## Measuring and reporting (the `any360-perf-step` skill)

- At every step, report the original against the native: time per call and memory per call (bytes and allocations,
  BenchmarkDotNet style), and in game the frame rate with its median and 10% / 1% lows plus the process's private
  memory. Never trade 1% lows for the average: lows are stutter.
- A/B in game with enough launches (5 or more a variant, alternating, same route), on the report's periods without
  diagnostics (`tools/frame_lows.py`), with a rank test before calling a difference real.
- Publish the comparison at the end of each step (`tools/d3d_perf.py` -> docs/D3D_PERF.md) and archive every
  before/after (raw reports, A/B outputs) under `docs/perf/archive/` with the date and the step: they are kept for
  later optimization passes.

## Repository and sessions

- Several sessions share these checkouts. Fetch and rebase on `origin/main` before every push; push with
  `git push origin HEAD:main`. Never stash, commit or revert files another session left (for example
  `docs/GLYPHS.md` while it is untracked). Commit only your own files.
- Commit messages end with the co-author line the session's instructions give.
- Run the whole suite (`layer\sys\test.ps1`) before committing; say so if anything fails. GPU changes run it with
  `-Gpus NVIDIA,Radeon`: the GPU tests run again under the Vulkan validation layer on each GPU and must give 0
  validation messages (one driver lets through what another refuses).
- When the owner splits the machine between sessions, each keeps to the cores and the GPU it was given (for
  example the GPU session: cores 8-15, the AMD Radeon): builds and tests through `start /affinity <mask>`, game
  launches with `tools/any360-ab.ps1 -Affinity <mask> -Device <name>`, and its own worktree and build folders so
  that builds, executables and logs never collide.
- Delete captures, dumps and replay outputs once they are no longer needed; keep only reference captures.
- For rendering decisions, show side-by-side images (original | before | after, same crop), not only percentages.

## Game launches

- Batch as many checks as possible into each launch; bisect by halves when something fails.
- Warn the owner before a visible launch. Minimized launches are for measuring only (they get no gamepad input).
- Never capture the screen to grab a game window (it captured the owner's desktop once).
- After each run, stop only the stray processes your own launch arguments identify.
- Slow frames: `--any360_hitch_trace_ms=<ms>` writes per-thread stack samples of frames over the threshold
  (`hitch_<n>.txt` in the logs); the report's slowest-frame line also says what `any360::sys` did in that frame.
- How to build and launch each title: the title repository's docs (Crackdown: `docs/ANY360.md` in its repository,
  `F:\Crackdown\crackdown-any360-sys` here, with `tools/any360-ab.ps1` for A/B launches).
