# any360::d3d: the titles' D3D runtime, native

Decision (user, 2026-11-12): no legacy Xbox translation layer is left. Today a title's statically linked XDK D3D
runtime (recompiled PowerPC) builds Xenos command streams (PM4) in guest memory, and Any360's command processor
(`layer/gfx/src/command_processor.cpp`) decodes them back into draws for the Vulkan renderer. The runtime is to be
replaced by native code that drives the renderer directly, every entry point of it, and the game-engine code that
writes GPU commands itself too. Asked between that and a hybrid that intercepts only the flush points, the user chose
the complete replacement, Crackdown first.

## What there is to replace (`tools/d3d_inventory.py`)

Crackdown's runtime, 0x826ACC90-0x826CF4B0 (`titles/crackdown/d3d_inventory.json`):

| Class | Functions | Instructions | Call sites from the title | What they are |
|---|---|---|---|---|
| flush | 64 | 12,708 | 177 | build PM4 or call the Vd* exports: draws, clears, resolves, swaps, waits, state flushes, command buffer replay, ring setup |
| entry | 163 | 7,406 | 2,292 | called by the title, build no PM4: state setters, resources, getters |
| internal | 326 | 14,913 | 0 | reached only from inside the runtime |

Outside the runtime, four functions build PM4 themselves: the engine's prebaked shader binds (`sub_828BB130`,
`sub_828BB308`, `sub_828BB4E8`: SET_STATE, IM_LOAD) and a wait just below the runtime's range (`sub_826AC8F0`). The
other titles (`tools/d3d_inventory.py tdu|simpsons`, their generated code cuts functions finer): Test Drive Unlimited
1,436 functions, 89 flush and 94 entry (372 call sites from the title), 7 functions outside that build PM4 (a draw among
them); The Simpsons Game 1,636 functions, 112 flush and 170 entry (2,317), 17 outside (a block of EA's engine,
`sub_825F8FE0`-`sub_825FABA8`). Each title links its own version of the runtime: the same functions, other
device offsets (the command buffer pointer at device + 48 in Crackdown, + 40 in Test Drive Unlimited).

Crackdown's studies before Any360 (`F:\Crackdown\HLE_RENDERER_FEASIBILITY.md`, `HLE_RENDERER_PLAN.md`) give the device
layout: a full Xenos register mirror with 64-bit dirty masks at +0 (vertex shader constants, 4 float4 a bit), +8
(pixel shader constants), +16, +24 (32 fetch slots), +32; float constants from +0x700; fetch constants at +1024 (24
bytes a slot); the command buffer's write pointer and end at +48/+52. Draw records read from that mirror after a draw
call matched the GPU's register state for 100% of the opaque scene's draws. About half of the scene's draws come from
recorded command buffers replayed by `sub_826C3750` (INDIRECT_BUFFER), not from draw calls.

## Design

**The device stays in guest memory, in the runtime's layout.** Title code reads and writes it directly (the XDK's
inline D3D functions compile into the game), so the native functions keep it exactly as the original ones do: the
register mirror, the dirty masks, the resources' headers. A title's layout is a table (`titles/<title>/d3d.json`:
the functions by name and address, the device's offsets), the code that uses it is Any360's and the same for every
title.

**Three kinds of replacement, each checked its own way:**

1. *Entry functions* (setters, resources, getters) are transforms of guest memory and registers, like the C runtime's
   functions of `docs/NATIVE_FUNCTIONS.md`: a model, unit tests, and `--any360_xdk_verify` in game (both sides run,
   memory and registers compared). Exact by construction, one group at a time, the game playable after each.
2. *Flush functions* produce renderer work instead of PM4. Checked against the PM4 path as an oracle: in a compare
   mode both run, the native function's draw (its `DrawCall`: registers, shaders, fetch) is compared with the one the
   command processor decodes from the original function's PM4 for the same draw, and the PM4 one is rendered.
3. *Engine code that builds PM4* (Crackdown's prebaked shader binds) is title code: replaced per title, the same way.

**Order while both paths exist.** Until every flush function is native, native draws and PM4 from functions not
replaced yet must reach the renderer in the order the title issued them. A native flush function writes, where its
PM4 would have gone, one Any360 packet (a type-3 opcode no Xenos packet uses) carrying the index of a native record;
the command processor runs the record when it reaches the packet. Fences, waits and the ring's read pointer keep
working as they do. Once every flush function of a title is native the ring holds only these packets; then it goes:
records go straight to the renderer's queue, fences are written when the GPU finishes the work they follow.

**Command buffers.** The runtime records a command buffer by pointing its write pointer into the buffer while the
title makes ordinary D3D calls; replay is an INDIRECT_BUFFER. Native: recording appends records to the buffer's list,
replay submits the list (with what the replay sets around it: predication, the inherited state).

**The renderer's input.** First the same `DrawCall` the command processor builds (registers, shaders), filled from the
device's mirror: exact by construction, and the renderer is unchanged. Later a leaner record (per-draw state blocks
shared between draws) once everything is native.

## Testing without playing: the self-test

`--any360_d3d_selftest=true` (layer/host/rexglue/src/d3d_selftest.cpp) turns a title launch into a test bench: from
the title's 2000th D3D call (its first menu frames: the ring is up), every case a title registers (`ANY360_D3D_CASE`
with its own addresses; inputs in `any360/host/d3d_cases.h`) runs the title's original function and Any360's
replacement 3000 times on generated inputs, on a copy of the device in the self-test's own memory, and compares what
each leaves there, in its scratch memory, on the caller's stack and in r3 (not for functions returning nothing). Then
both are timed, 8 calls in a row an input (the clock's tick can be 10 ns), and the medians written. The work goes in
slices of a few milliseconds a frame on the title's D3D thread, one thread at a time, so the title keeps drawing; the
results go to the log and `d3d_selftest.txt`, and the process ends by itself (Crackdown: 24 cases, about 3 minutes,
minimized, no input).

What a case must never do: let the original reach a path that writes the title's real state through a pointer the
device copy still holds (making room in the command buffer, a new release-list block), give an upload a mask of 0 or
a count the runtime loops on, or a range whose end is below its start.

## Reaching a title's game without a player

`--any360_input_script="12:start,16:a,20:a,24:a"` (any360::sys InputScript) adds button presses to the first
controller's samples at set times, so its state and keystrokes both see them: Crackdown reaches the city from the title
screen with Start then A every 4 s, minimized, no one at the controller.

## Phases

| # | Phase | Done when |
|---|---|---|
| D0 | Done (2026-11-12). Inventory: `tools/d3d_inventory.py`, `titles/crackdown/d3d_inventory.json`. Roles: `tools/d3d_roles.py` writes `titles/crackdown/d3d.json`, each of the 227 entry and flush functions with its argument registers, the offsets it reads and writes in r3's object, its PM4 and calls, and a name (`titles/crackdown/d3d_names.json`: 37 read so far, the most called) or a role made from those | every entry and flush function of Crackdown's runtime has a name and a role |
| D1 | In progress: 22 bound native in Crackdown (blend, constants, SetTexture, SetStreamSource, samplers, vertex declaration, shader binds, CPU range flush: `any360/xdk/d3d_state.h`), each checked by the in-game self-test (`--any360_d3d_selftest`) and `--any360_xdk_verify`, every one at least 1.1x its original. Entry functions native, group by group (constants and fetch setters first: `sub_826B2F68` is already one, `xdk::D3dSetShaderConstants`), verified in game | the 163 entry functions run natively in Crackdown, `--any360_xdk_verify` clean |
| D2 | In progress (2026-11-12): the record packet is `any360/xdk/d3d_records.h` (a type-3 header, opcode 0x7E, then words in the host's order: registers, draws), executed by the command processor (`ExecuteRecord`, unit-tested against the PM4 it replaces); DrawIndexedVertices (`sub_826B9FF8`) native in `any360/host/d3d_draw.h`, its dirty state flushed through the runtime's upload helpers and its draw written as a record (`--any360_d3d_records`, default on; `--any360_d3d_native_draws`), compared with the runtime's code by the self-test (PM4 byte for byte, records through a canonical decoding), and in the city: the same 2,326 draws a frame, 0 failed. Since 2026-11-13 all four draw flushes are native: DrawVertices (`sub_826B9C08`), BeginVertices (`sub_826B91E0`) and BeginIndexedVertices (`sub_826B96D0`, their blocks taken through the runtime's allocator) too, 9 self-test cases 0 different (1.2-1.7x), the city's 2,324 draws a frame unchanged. Next: clears, resolves, swaps, fences (D3). The record packet in the command processor and the renderer; the first flush function native (indexed draw `sub_826B9FF8`), compare mode against PM4 | its draws match the PM4 ones, the image unchanged |
| D3 | In progress (2026-11-13): SetPredication, query Issue, Clear (`sub_826BFD88`) and Resolve (`sub_826BE578`) native; records for bins, events, shader loads, invalidates, waits; the draws' dirty uploads, fetch-constant (`sub_826C5D40`), wait-group (`sub_826C5BC0`) and state (`sub_826C5768`) helpers native, writing records; the shader flush (`sub_826C6C48`) native with its shader control (`sub_826C5580`), literal constants (`sub_826C6A50`, records `kConstantLoad`) and patched-copy cache (`sub_826C6B00`'s hits), shader loads as `kShaderLoad` records: 83% of its calls in the city, the rest (a vertex shader copy patched again into the command buffer: `sub_826C68E0`, the microcode patcher `sub_826C6010`) still the runtime's. Left: that patcher, the occlusion helper `sub_826C05C8`, tiling `sub_826BD488`; the swap `sub_826BCA98` belongs with any360::sys VdSwap. Every draw, clear, resolve, swap, query and fence native | no PM4 from the runtime but command buffer replays and ring setup |
| D4 | Command buffers recorded and replayed as record lists | no PM4 from the runtime at all |
| D5 | The engine's PM4 (`sub_828BB*`) native | the ring carries only record packets |
| D6 | The ring and the command processor out of Crackdown's path (kept as the oracle for the other titles and for checks) | Crackdown runs with no PM4 built or decoded |
| D7 | Test Drive Unlimited, The Simpsons Game, Call of Duty: Black Ops: their layouts and their engines' PM4 | each runs with no PM4 |

D0-D6 are months of work. The game stays playable after every step: what is not native yet runs as before.
