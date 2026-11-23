# 2026-11-23: Mirror's Edge binds the D3D GPU wait step

Item 2 of [the Mirror's Edge 1440p list](../2026-11-23-mirrorsedge-1440p-profile/NOTES.md): the game's threads
spun in the D3D runtime's GPU waits, `sub_827FF5D0` 10.0 s + `sub_82813C40` 7.5 s of own time over 30 s (0.58 of a
core). `sub_827FF5D0` is the runtime's GPU wait step, the same code as in Crackdown, The Simpsons Game, Test Drive
Unlimited and Call of Duty: Black Ops (eight `db16cyc`, the device's stop flag at +10941, the ring's read pointer
through +10896, the hang check through `sub_82819318` after 5000 ms); `sub_82813C40` is the ring-space wait looping
on it. Mirror's Edge now binds Any360's shared native (`ANY360_XDK_BIND(827FF5D0, D3dGpuWaitStep)` in the port's
`src/xdk_natives.cpp`): the step's own code, then a sleep on the GPU's progress count (any360_d3d_wait_sleep). Its
model and unit tests are those of `xdk::D3dGpuWait` (`xdk_d3d_tests`); nothing new in Any360 beyond the database
entry.

## Before and after

Same build (Any360 a54790c, the k_16_16_16_16 resolves on the GPU), 1440p host MSAA 4x, cores 0-7, first level,
the profile started with F6 in game. A: `--any360_xdk_natives=false` (the step's own code alone); B: bound.

| | Before item 1 (CPU resolves) | A: not bound | B: bound |
|---|---|---|---|
| `sub_827FF5D0` + `sub_82813C40` own time, 30 s | 17.5 s (0.58 of a core) | not sampled once (0 of 106131 samples) | no profile (the run ended before the 30 s) |
| Wait steps | spinning | spinning when they run | 72-325 every 10 s, all slept, 2-9 to the 2 ms time-out |
| FPS in game (periods without diagnostics) | 34-40 | 62 (the game's cap), 1% low 45-54 | 62, the same draws and resolves |

Item 1 removed the cause: the GPU side is no longer late, so the waits barely run and their spin had disappeared from
the profile before the binding. The binding is kept as the generic native it is in the other titles (at no cost when
nothing waits; it frees the core when the GPU is late, as on smaller hardware). The profile without the binding
([profile_binding_off.txt](profile_binding_off.txt)) shows where the game's threads go now: `sub_82A01840` 6.7 s
(5.7 s of it sleeping in `KeDelayExecutionThread`) and `sub_829F9470` 4.3 s of its own (reading the clock), likely
the game's frame pacing at its 62 FPS cap.

Single launches each, short (about a minute of play): an in-game check, not an A/B of the frame rate.
