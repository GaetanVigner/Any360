# Any360's D3D records only while its graphics are bound (2026-11-27)

**Bug.** A title running on the runtime's own GPU (no `--any360_gpu=true`) with Any360's native D3D draws on (their
default) wrote Any360's command records (type-3 opcode 0x7E, `any360/xdk/d3d_records.h`) into its command buffers.
Only Any360's command processor reads them. ReXGlue's GPU logged `Unimplemented GPU OPCODE: 0x7E` and
`INDIRECT RINGBUFFER: Failed to execute packet`, and drew nothing more (Crackdown, Windows and Linux: hung on its first
frames). `before_crackdown_486.log` is such a launch, 2026-11-26.

**Change.** `d3d_draw::DrawRecords()` (every record writer asks it) is `any360_d3d_records && GraphicsBound()`. Unbound,
the same natives write the runtime's PM4, the form they already wrote with `any360_d3d_records` off (the self-test
compares both forms). `host_rexglue_tests` checks it is off before `StartGraphics` and on after.

**In game** (Crackdown, Any360 45ebd3a + this change, city route, uncapped, one launch each, cores 0-7, RTX 5070 Ti):

| launch | arguments | packet errors | city FPS (game swaps) | 1% low |
|---|---|---:|---:|---:|
| crackdown_510 | `--any360_gpu=false --any360_sys_groups=input` (natives at their defaults) | 0 | 295-301 | 206-248 |
| crackdown_511 | Any360 (8 sys groups, `--any360_gpu=true`) | 0 | 199-219 | 141-148 |

The runtime's GPU with Any360's natives now runs (faster than with them off, 269 FPS on 2026-11-26's bench). Any360's
launch is unchanged by this change (the bound path keeps records), but it ran at about 210 FPS against 288 on
81b3a4b the day before: its command processor took 2.9 ms a frame against 2.5 and its draws 3.4 ms against 2.6, with the
same draws a frame. That comes from another cause (main's commits since 81b3a4b, or the machine's load at that time),
and is reported to the command processor's owner.

Suite: `layer\sys\test.ps1 -Gpus NVIDIA,Radeon`, 0 failing checks, 0 validation messages.
