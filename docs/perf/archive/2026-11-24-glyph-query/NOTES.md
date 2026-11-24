# The glyph query (docs/GLYPHS.md section 3), 2026-11-24

`any360/sys/input_glyphs.h`: controller families from the host's pad description, the active device per user (real
input only, 250 ms debounce, hand-over on unplug), and `GlyphQuery::For(user, control, at)` answering a glyph set and a
control name through the bindings (`ResolveProfile`, shared with layer 1's mapper).

`sys_input_tests --bench` (bench.txt), 10 M calls each, cores 0-7, RTX 5070 Ti machine:

| Call | ns a call | Allocations a call | Bytes a call |
| --- | --- | --- | --- |
| `GlyphQuery::For` | 2.59 | 0 | 0 |
| `ActiveDevice::Input` + `Shown` (two devices alternating) | 2.73 | 0 | 0 |
| `FamilyOf` | 2.09 | 0 | 0 |
| `IsRealInput` | 1.69 | 0 | 0 |
| `InputMapper::Map` (unchanged, for reference: identity / 8 targets) | 5.54 / 33.82 | 0 | 0 |

No original to time: a title draws its prompts from its own data and has no such call (an adapter will redirect its
choice here, section 4), and the host has no equivalent (SDL gives the pad type and labels, which `FamilyOf` reads).
The tables a query reads are built once per bindings file (`SetBindings`: one short string per family and control,
10 x 20); an answer only reads them.
