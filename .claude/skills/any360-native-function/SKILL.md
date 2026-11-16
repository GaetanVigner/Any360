---
name: any360-native-function
description: The steps to make one of a title's functions native in Any360 (XDK CRT, D3D runtime, kernel) - read it, model it, test it against the original, bind it, register it, measure it. Use whenever a title function is replaced by Any360 code.
---

# Making a title function native

Follow `docs/NATIVE_FUNCTIONS.md` (the written rule) and, for the D3D runtime, `docs/D3D.md`. In order:

1. **Read the original** in the title's recompiled code (`generated/default/*_recomp.*.cpp`, the PowerPC is in the
   comments) and every function it calls. Note what it reads, writes (including the caller's parameter area and the
   stack below r1), returns, and which registers its callees receive (leftover volatile registers flow into callees).
2. **Write the native** (Any360 headers: `layer/xdk/include/any360/xdk/` for pure models, `layer/host/rexglue/include/
   any360/host/d3d_draw.h` for D3D natives bound in titles). Validate every input before the first write and decline
   (the original then runs) on anything outside what was modelled. Call the title's helpers through the `Runtime`
   table the title fills, with r1 set to the original's frame. Allocate nothing per call (reuse buffers, pools).
3. **Test it**: a unit test of the logic where it can run off-game (`layer/xdk/tests`, `layer/gfx/tests`), and an
   in-game self-test case (`layer/host/rexglue/include/any360/host/d3d_cases.h`, registered in the title with
   `ANY360_D3D_CASE`) comparing every byte against the original on random inputs: 0 different.
4. **Bind it** in the title (`CRACKDOWN_BIND_D3D(<address>, <Name>OrOriginal)` or the title's own hook calling
   `<Name>OrOriginal`), behind a runtime switch while it is new.
5. **Register it**: `db/xdk_seed.json`, `layer/sys/native.json`, `titles/<title>/d3d_names.json` (one entry a line),
   then `tools/build_db.py`, `tools/progress_page.py`; regenerate the title's progress reports
   (`tools/title_progress.py <title> --out <repo>/docs/progress --readme <repo>/README.md`).
6. **Measure** with the `any360-perf-step` skill: time and memory per call against the original (at least 1x, no
   allocation), in-game A/B with lows, published and archived.
7. **Commit** Any360 (suite green, rebased on `origin/main`) and the title (its bindings and regenerated reports),
   and tell the owner the before/after.
