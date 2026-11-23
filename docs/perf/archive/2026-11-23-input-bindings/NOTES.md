# 2026-11-23: input bindings, layer 1 (docs/GLYPHS.md, phase 1a)

The launcher's bindings file (`--any360_input_bindings`, JSON, `any360/sys/json.h` and `input_bindings.h`) and
`InputMapper`, applied to every controller's sample in `Xam::Sample` before the input queue: physical controls by
position -> the Xbox 360 controller the title reads. Cost of one sample (`sys_input_tests --bench`, 10 million
samples, cores 0-7, three runs, [bench.txt](bench.txt)):

| | ns a sample | allocations a sample | bytes a sample |
|---|---|---|---|
| No bindings (the identity, today's path) | 5.5-5.6 | 0 | 0 |
| 8 targets bound (A/B and X/Y swapped, LB/LT swapped, right Y inverted, up from the D-pad or the stick) | 38.5-39.8 | 0 | 0 |

There is no original to compare: a title never remaps its controller (the runtime's input system reports the pad as
it is). At the 1 kHz sampling rate, 8 bound targets cost about 0.04 ms a second a controller. Building the mapper
allocates once (its sources), when the file is read.

Tests: `sys_input_tests` (the JSON reader against RFC 8259: values, escapes, surrogate pairs, 20 malformed documents,
error positions, the nesting bound; the bindings file: every control name, every kind of target and source, 7 bad
entries reported; the mapper: identity, swap, trigger as a button with hysteresis, button as a trigger, button as an
axis, stick direction as a button with hysteresis, inversion, base profiles and a loop of them, the keyboard profile
left to phase 1b, the inverse for the glyph query) and `sys_object_tests` (A/B swapped through `XamInputGetState`,
the identity again once cleared).

Not done yet: the in-game check (a swap felt in a menu of two titles), phase 1b (keyboard and per-device profiles).
