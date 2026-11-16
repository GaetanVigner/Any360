---
name: any360-perf-step
description: How to measure, compare and publish the performance of an Any360 step (time and memory per call, in-game frame rate with median and 1%/10% lows, process memory) and archive every before/after. Use at the end of every step that touches native code or the renderer, and whenever the owner asks for performance figures.
---

# Measuring and publishing an Any360 step

Every step that lands or changes native code or the renderer ends with these measurements, reported to the owner
(in French) and published in the repository. The owner reads lows and memory as closely as averages.

## 1. Per call: time and memory against the original

- The in-game D3D self-test (`--any360_d3d_selftest=true`, `--any360_d3d_selftest_only=<name texts, comma
  separated, no spaces>`) runs each case the title's way and Any360's on the same inputs and reports, per case: calls,
  declined, different, median ns of each side and the ratio, and the host memory each side allocated per call
  (bytes and allocations). The report lands in the build folder as `d3d_selftest.txt`.
- Off-game benches (`xdk_*_bench`, timings printed by `gfx_*_tests`) report time and allocations per call the same way.
- Rules: 0 different; native at least 1x the original; 0 bytes allocated per call in steady state unless there is a
  reason (say it). Prefer pooled buffers and reused vectors to fresh allocations.
- A function the self-test cannot run (it submits to the GPU, kicks, waits) is checked by in-game A/B (section 2) and
  covered by a unit test of its logic.

## 2. In game: frame rate, lows, memory

- Launch the title minimized on a scripted route (its `CLAUDE.md` gives the command and input script), 5 or more
  launches a variant, alternating variants, 100 s or more each. Use the runtime switches of the step (for example
  `--any360_native_tiling`, `--any360_direct_submit`) or two executables (keep the "before" build next to the new one).
- Analyze with `python -I tools/frame_lows.py <runs file> <logs dir>`: it reads the report periods without
  diagnostics (the periods alternate; the ones with diagnostics are noisier), splits each launch's dip (lowest 1% low:
  streaming, loads) from its steady periods, gives FPS / 10% low / 1% low / median, the dips' slowest frames and
  texture loading, the process's private memory, and a rank test (z; past about 2, a real difference).
- Report averages and lows together. A change that raises the average but lowers the 1% low is a regression.

## 3. Publish and archive

- `python -I tools/d3d_perf.py <title> <d3d_selftest.txt> --note "..." --archive <step-label>` regenerates
  `docs/D3D_PERF.md` and copies it with the raw report into `docs/perf/archive/<date>-<step-label>/`.
- Save the A/B runs file and the `frame_lows.py` output into the same archive folder.
- Keep everything: the archive is how a later pass knows what each step changed.
- Mention the published numbers in `docs/D3D.md`'s step text, and tell the owner the before/after (time, memory,
  FPS, lows) in the reply.
