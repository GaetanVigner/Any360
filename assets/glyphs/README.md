# Glyph sets

Any360's own button glyphs (docs/GLYPHS.md, section 3): one set per controller family, the same art for the launcher
and the games. The pad art comes from the pack first drawn for crackdown-xbox360-recompiled (dark disc, grey ring,
Segoe UI Bold labels); every family is drawn in that style. Generic shapes only, no platform logos: the home button
of every family is a globe.

| File | What it is |
| --- | --- |
| `svg/common/*.svg` | Shared by the families: sticks (and their 8 directions), D-pad (and its 8 directions), home. |
| `svg/<family>/*.svg` | What differs per family: face buttons, shoulders, triggers, start/back, stick clicks. |
| `families.json` | The index the generator reads: each family's SVG per control, `absent` controls. |
| `<family>_128.png`, `<family>_64.png` | The atlases: 8-bit RGBA, straight alpha, fully transparent pixels black. |
| `<family>.json` | Each control's cell (`column`, `row`, the same at every size) and the atlases' sizes. |
| `preview.png` | Every family's 128 px atlas over grey, for humans. |

Control names are the `PadControl` names of `layer/sys/src/input_bindings.cpp` (`south`, `left_shoulder`, `left_y+`,
...), plus prompt-only names: `dpad`, and `dpad_`/`left_`/`right_` with `up_left`, `up_right`, `down_left`,
`down_right`.

Families: `xbox` (A/B/X/Y, LB/RB/LT/RT, menu/view), `ps3` (cross/circle/square/triangle, L1/R1/L2/R2, L3/R3,
START/SELECT). Next: PS4/PS5 (Create/Options, touchpad), Switch, Steam Deck, keyboard and mouse (the Crackdown pack's
keycaps).

## Regenerating

After editing or adding an SVG (and its line in `families.json`):

    python -I tools/make_glyphs.py

It needs Microsoft Edge or Chrome; the rest is the Python standard library. It refuses an unknown control name, a
family missing a control it does not declare absent, and a missing or unreferenced SVG. Render on Windows to reproduce
the committed atlases exactly (fonts).
