# 2026-11-27: Mirror's Edge binds 19 of the natives Any360 already has

Mirror's Edge had 26 functions Any360 has native and the title did not bind (docs/ROADMAP.md). Each was checked
against the title's own body, run both ways in game, measured, then bound or set aside. Machine: Ryzen 7 7800X3D,
RTX 5070 Ti; route2 replayed (the first level from launch), minimized. All 16 logical processors were in use (the
runtime resets the launch's affinity mask): both sides of each comparison ran in the same process, so the ratios hold.

## Bound (MirrorsEdgeRewrite src/xdk_natives.cpp)

| Function | Address | Handler | Checked in game | Own code -> native (in game) |
|---|---|---|---|---|
| memcpy's plain forward copy | 829E3C48 | CrtCopyForward | 2,379 calls, 0 different | 58.5 -> 36.3 ns (1.6x) |
| memset | 829E55B0 | CrtMemset | 3,999,428 | 37.8 -> 15.5 ns (2.4x) |
| wcslen | 829E3F38 | CrtWcslen | 1,690,695 | 39.9 -> 29.2 ns (1.4x) |
| wcscmp | 829E5DD0 | CrtWcscmp | 2,755,522 | 29.5 -> 17.3 ns (1.7x) |
| wcscpy | 829E3F18 | CrtWcscpy | 98,379 | 54.0 -> 35.3 ns (1.5x) |
| wcsncpy | 829E8940 | CrtWcsncpy | 97,485 | 93.7 -> 36.7 ns (2.5x) |
| strncpy | 829E6738 | CrtStrncpy | 187 | 74.7 -> 26.2 ns (2.9x) |
| strncmp | 829E7E60 | CrtStrncmp | 8 | 767 -> 72 ns (10.6x, 8 calls) |
| strrchr | 829F40C0 | CrtStrrchr | 4 | 87 -> 72 ns (1.2x, 4 calls) |
| strstr | 829E7000 | CrtStrstr | 4 | 1,952 -> 140 ns (14x, 4 calls) |
| _stricmp | 829E9650 | CrtStricmp | 1,132 | 15.0 -> 11.0 ns (1.4x) |
| mbtowc | 829F4F48 | CrtMbtowc | 30,875 | 11.0 -> 10.9 ns (1.0x) |
| _isnan | 829E8DD8 | CrtIsnan | 64,888 | 16.8 -> 16.5 ns (1.0x) |
| _get_exp | 829EE070 | CrtGetExp | 32,323 | 25.9 -> 18.4 ns (1.4x) |
| D3DResource_GetType | 827FEC40 | D3dState<ResourceType> | 62,077 | 17.6 -> 13.8 ns (1.3x) |
| D3D_GpuFormatBlock | 828106A0 | D3dState<FormatBlock> | 164,614 | 17.8 -> 14.0 ns (1.3x) |
| D3DResource_AddRef | 827FEAE0 | d3d_draw::ResourceAddRefOrOriginal | D3D self-test 3,000, 0 different | 28.8 -> 8.8 ns (3.3x, self-test) |
| D3DResource_Release | 827FEB58 | d3d_draw::ResourceReleaseOrOriginal (destroy: sub_827FE408) | self-test 3,000, 0 | 25.0 -> 10.0 ns (2.5x, self-test) |
| D3D_LinkShaders | 82816A00 | d3d_draw::LinkShadersOrOriginal (new) | self-test 3,000, 0 | 216 -> 91 ns (2.4x, self-test) |

- The check in game (`--any360_xdk_verify=true`, `verify-final.txt`): 9.0 M calls of the bound functions, 0 different.
  Bytes next to a destination changed while memset and wcsncpy ran (other threads' writes), counted and not compared.
- The measure in game (`--any360_xdk_bench=true`, `bench.txt`), means a call over the route.
- The D3D self-test (`--any360_d3d_selftest=true --any360_d3d_selftest_after=20`, started from the GPU wait step,
  whose r3 holds the device): run 1 found 2 different calls of the format block and 1 of the shader link, in the
  physical view the self-test's cases share; run 2, on the same fixed-seed inputs, 0 different for all five. The
  functions themselves are deterministic on those inputs: another writer of that memory, not the natives
  (`d3d_selftest-run1.txt`, `d3d_selftest-run2.txt`). The format block also passed 164,614 calls in game.
- All bytes and allocations a call: 0 (the self-test's figures; the CRT natives allocate nothing by design).
- Addresses all inside titles/mirrorsedge/modules.json's library ranges.

## Not bound

| Function | Why |
|---|---|
| D3DDevice_SetRenderState_ViewportEnable, D3DDevice_DrawVerticesUP | Same instructions as Crackdown's with the device's fields elsewhere (10572 and 13444 against 10444 and 13076): the natives read Crackdown's layout (xdk::g_d3d_layout). Until the layout is a title's parameter. |
| strchr | Not called in any run of the route. |
| memmove (not_worth) | 2,424 -> 2,855 ns a call on average (0.8x): faster in 18 of its 22 size classes, slower on empty moves (28 -> 34 ns, 26,487 calls) and on three classes of large copies (16-32 KB, 64-128 KB, 512 KB-1 MB) whose time dominates. |
| _finite (not_worth) | 10.2 -> 11.1 ns (0.9x). |
| GetLastError, SetLastError (not_worth) | Not worth binding (the rule of 2026-11-27); measured here 1.2x and 1.6x on the 5-instruction functions, within the binding's own cost. |

## Fixed on the way: the bench mode's slow-call re-run

`--any360_xdk_bench=true` crashed Mirror's Edge (`bench-crash-before-fix.txt`: D3dState<ResourceType> reading guest
address 3). When a native call took over ten times its own code's time, RunSides ran it again on `ctx`, which by then
held the call's result (r3 = the type, 3) instead of its arguments. It now runs again from the registers the call came
with. No unit test reaches the bench mode's re-run (it needs a slow native call in a running title).

## D3D_LinkShaders on its own

d3d_draw::LinkShadersOrOriginal: the shaders' link (Crackdown sub_826C67A0) as a binding of its own, behind
any360_d3d_native_draw_helpers like the other draw helpers; Crackdown links through its flush path and binds no
standalone copy.
