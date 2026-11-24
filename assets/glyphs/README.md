# Glyph sets

Any360's own button glyphs (docs/GLYPHS.md, section 3): one set per controller family, the same art for the launcher
and the games. The pad art comes from the pack first drawn for crackdown-xbox360-recompiled (dark disc, grey ring,
Segoe UI Bold labels); every family is drawn in that style. Generic shapes only, no platform logos: the home button
of every family is a globe.

| File | What it is |
| --- | --- |
| `svg/common/*.svg` | Shared by every family: sticks (and their 8 directions), D-pad (and its 8 directions), home, ▶/◀. |
| `svg/<group>/*.svg` | Art shared by a group (`xbox`, `playstation`) or one family's own (`ps3`, `ps4`, `ps5`, `switch`, `steam`). |
| `families.json` | The index the generator reads: shared blocks, and each family's blocks (`use`), own SVGs and `absent` controls. |
| `<family>_128.png`, `<family>_64.png` | The atlases: 8-bit RGBA, straight alpha, fully transparent pixels black. |
| `<family>.json` | Each control's cell (`column`, `row`, the same at every size) and the atlases' sizes. |
| `preview.png` | Every family's 128 px atlas over grey, for humans. |

Control names are the `PadControl` names of `layer/sys/src/input_bindings.cpp` (`south`, `left_shoulder`, `left_y+`,
...), plus prompt-only names: `dpad`, and `dpad_`/`left_`/`right_` with `up_left`, `up_right`, `down_left`,
`down_right`.

Faces are by position, as the bindings are: on a Switch pad `south` is the button printed B.

| Family | Faces | Shoulders / triggers / clicks | start / back | Extra |
| --- | --- | --- | --- | --- |
| `xbox360` | A B X Y | LB RB / LT RT / L R | ▶ / ◀ | |
| `xbox_one` (One, Series) | A B X Y | LB RB / LT RT / L R | menu / view | |
| `ps3` | cross circle square triangle | L1 R1 / L2 R2 / L3 R3 | START ▶ / SELECT ▬ | |
| `ps4` | as PS3 | as PS3 | OPTIONS / SHARE | touchpad |
| `ps5` | as PS3 | as PS3 | options ≡ / create | touchpad |
| `switch` (Pro, Joy-Con pair) | B A Y X (south east west north) | L R / ZL ZR / L R | + / − | |
| `steam_controller` | A B X Y | LB RB / LT RT / L R | ▶ / ◀ | right trackpad as the right stick |
| `steam_deck` | A B X Y (white) | L1 R1 / L2 R2 / L3 R3 | menu / view | |
| `keyboard_mouse` | the Crackdown pack's keycaps and mouse glyphs | | | `key:wasd`, `key:arrows`, `mouse:move` |

Shared art: `common` (sticks, D-pad, home, ▶/◀), `xbox` (coloured A/B/X/Y, LB-RT, menu/view), `letters` (white
A/B/X/Y: Switch, Deck), `numbered` (L1-R3: PlayStation, Deck), `playstation` (shapes, touchpad).

The keyboard family is named as the bindings file writes a source: `key:<name>` (`a`-`z`, `0`-`9`, `f1`-`f12`,
`backtick minus equals comma period semicolon slash backslash left_bracket right_bracket quote escape enter space tab
backspace delete insert home end page_up page_down left right up down shift control alt caps_lock`, `numpad_0`-`9`,
`numpad_enter numpad_plus numpad_minus numpad_multiply numpad_divide print_screen pause num_lock scroll_lock`) and
`mouse:<left right middle wheel wheel_up wheel_down x y>`; the list is `KEYBOARD_NAMES` in `tools/make_glyphs.py`.

## Regenerating

After editing or adding an SVG (and its line in `families.json`):

    python -I tools/make_glyphs.py

It needs Microsoft Edge or Chrome; the rest is the Python standard library. It refuses an unknown control name, a
family missing a control it does not declare absent, and a missing or unreferenced SVG. Render on Windows to reproduce
the committed atlases exactly (fonts).
