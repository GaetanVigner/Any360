# Input bindings and glyphs (plan)

Status (2026-11-23): plan decided (section 8); being built from phase 1 (the bindings file and `InputMapper`,
section 2). Revised 2026-11-23: two binding layers (the title's controller, and per title its actions), the title's
own remap replaced by Any360's (C7), the launcher as the one place bindings are kept (C9).

**The goal:** a player rebinds in Any360Launcher and the game shows the right glyphs. No input a title reads stays
hard-wired to a physical control, and a title's own remap is replaced by Any360's.

The goal is the one modern platforms already meet: Steam Input's action glyphs, SDL 3's gamepad types and button labels,
the GDK's and PlayStation's button prompts. For a title running on Any360:

- if the player swaps A and B in Any360Launcher, every in-game prompt that said "Press A" now shows B's glyph, and
  pressing that physical button does what A did;
- prompts follow the controller in the player's hands: Xbox, PlayStation, Switch, Steam Deck, keyboard and mouse;
- no input is lost on the way, whatever the frame rate (the queue built for The Simpsons Game, section 1).

### Scope now: controller glyphs

This plan covers the **controller glyphs**: the button images a prompt shows (A, B, triggers, sticks, D-pad...) for
every pad family, following the bindings and the device. Not in it yet:

- **Text fonts** (a title's typefaces, the letters around the glyphs): later, they are even more case by case.
- Keyboard and mouse prompt glyphs come right after the pad families (the keycaps exist in the Crackdown pack); the
  keyboard and mouse *bindings* and their detection are in scope from the start, since switching device switches the
  bindings.

### Generic, per title, and fine-tuning

This is the first Any360 feature that **cannot be generic end to end**. Input, bindings, device detection and the
glyph query work for any title with no title code. But a title picks its prompt art in its own code (a font's glyph
records in Crackdown, text tokens in The Simpsons Game, a Flash variable in TDU, the bind system in CoD), and only
reading and adapting that code makes its prompts follow the bindings. So:

- **Generic = the port.** What Any360 provides (sections 1 to 3, the mechanisms of section 4) *and* the adaptation of
  each title's prompt code to use it: a title adapter, written once per title when it is ported, that interprets how
  the original picks a glyph and redirects that choice to `GlyphFor`. Every ported title gets one; the adapter is part
  of the port, like the import binding table.
- **Fine-tuning = closer to a remaster.** Done later, per title, on top of a working port: new or higher-resolution art
  for a title's own glyphs, upscaled textures, text fonts, the title's action names in the launcher ("A: Jump").

Any360 keeps what is the same everywhere (the API, the mechanisms an adapter uses, their tests); the title's repository
holds its adapter and its data.

## 0. Where input goes today

```
device (SDL / XInput / keyboard+mouse driver of the ReXGlue runtime)
  -> driver folds its events into one X_INPUT_STATE per device      rex::input::*Driver
  -> InputSystem::GetState(user): devices merged (OR, max)           rex::input::InputSystem
  -> any360::sys samples it every 1 ms on its own thread             XamState::StartInputSampling
  -> InputQueue: every button/trigger change queued                  any360/sys/input.h
  -> XamInputGetState presents one step per console frame            (sticks live, keystrokes from every change)
  -> the title
```

`InputQueue` (`87b593f`, made for The Simpsons Game above 60 FPS) guarantees that every change it *samples* reaches the
title and stays visible one console frame. What it cannot see is what happens before its sample:

1. **The SDL driver folds events into a state.** `SDLInputDriver::OnControllerDeviceButtonChangedLocked` sets or clears
   a bit of `controller.state.gamepad.buttons`. Events are pumped on the UI thread (`QueueControllerUpdate` ->
   `CallInUIThread(SDL_PumpEvents)`) and drained in a batch: a press and its release in the same batch leave the state
   unchanged, the press is gone.
2. **The 1 kHz sampler is a poll.** A press shorter than the sampling period, or two changes between two samples, fold
   the same way. Rare at 1 kHz, but a poll is a poll: the period also trades CPU for accuracy.
3. **XInput and the keyboard/mouse driver are polled states too** (`XInputGetState`, `key_down_[256]`).
4. **Devices are merged before Any360 sees them**: which device produced an input (needed for glyphs, section 3) and
   per-device bindings (section 2) are lost at `InputSystem::GetState`.

## 1. Input path: events first, nothing lost

The standard on current platforms is an ordered stream of timestamped transitions, not a polled state: SDL 3's gamepad
and keyboard events carry a nanosecond timestamp and a device id; the GDK's GameInput keeps a history of readings per
device (`GetNextReading`); Steam Input delivers action events. Any360 takes the same shape:

```
device events (SDL 3 event watch / GameInput readings / polled fallback)
  -> InputEvent {device, physical control, value, timestamp}     host side: one adapter per backend
  -> InputMapper: physical control -> logical control (bindings)  any360::sys, section 2
  -> InputQueue::Push(logical change, timestamp)                  the Simpsons queue, unchanged semantics
  -> XamInputGetState / XamInputGetKeystroke                       one step per console frame, as today
```

- **The queue keeps its rules** (steps one console frame apart, inputs pressed together arrive together, a double tap is
  two presses, sticks live, keystrokes from every change). What changes is its feed: `Push` takes each transition with
  its own timestamp instead of `Sample` diffing polled states. `Sample` stays for backends without events.
- **Events are taken where SDL produces them**, with `SDL_AddEventWatch` (called on the thread that pumps, before any
  folding), not from the driver's folded state. A press and release in one batch become two queued changes.
- **The pump runs on its own schedule**, not only when the title polls: the sampling thread asks for a pump every
  period (today 1 ms), so event timestamps stay close to the hardware's and a title that polls rarely still sees
  every transition in order.
- **Polled backends** (XInput on Windows without SDL, a title's own driver) keep `Sample`: their loss bound is the
  sampling period, as today. SDL 3 is the event source on every platform, Windows included (C1); GameInput would only
  come in if SDL stopped being the backend.
- **Per device until the mapper.** The merge (OR of buttons, max of triggers and sticks) moves after the mapper, so each
  device is mapped with its own bindings and the active device is known (section 3).
- **The input script stays logical.** `InputScript` presses are guest buttons ("a" is the title's A): they join after
  the mapper, as today after the controller state.

### Several players (split screen)

A title reads up to four users (`XamInputGetState`'s user index), as four pads on the console. Any360 decides which
device is which user (`any360/sys/input_players.h`, `PlayerSlots`, built 2026-11-24), not the runtime's driver, so
that players never swap places mid-game:

- A pad takes the lowest slot neither occupied nor reserved; none left, the oldest reservation.
- A pad unplugged keeps its slot reserved for its identity (SDL's GUID and serial number, or its path): the others
  stay where they are, and the same pad plugged back takes its slot again. A new pad does not take a reserved slot
  while a free one is left.
- Pads beyond four wait in order and take a slot as soon as one frees.
- The keyboard and mouse join one player (the first by default, a launcher setting later) without a slot of their own.
- Everything after the slot is per user: the merge of a user's devices after the mapper (each device with its own
  profile), the user's queue, and the user's glyphs (`GlyphQuery::user(n)`: player 2's DualSense shows its own glyphs
  while player 1 types on the keyboard).

Tests (`sys_input_tests`): two pads and the keyboard; player 1's pad unplugged and back while player 2 stays user 1;
a new pad not taking a reserved slot; a fifth pad waiting then placed; the oldest reservation taken when all are;
each player's glyphs following their own devices.

On the host (`layer/host/rexglue`, `players.h`, 2026-11-24): Any360's `DeviceAssignment` is installed on the
runtime's `InputSystem` at the first controller read (`--any360_input_players=false` keeps the runtime's), so the
runtime asks `PlayerSlots` which devices feed which user and merges each user's own devices. A pad's identity is its
SDL GUID (and its rank among pads sharing it); its family comes from the GUID's vendor and product and its name
(`DescribePad`, a table of known pads, no SDL call: the title links its own SDL, not the one that opened the pads).
After each read the host tells the user's pad from the keyboard sharing it (the pad read alone through the same
assignment) and reports real input to the user's glyph query; `Xam` asks each user's family and maps them with its
profile. `host_players_tests` runs it on the runtime's own `InputSystem` with a fake driver: split screen, unplug and
back, the keyboard joining player 1, families and prompts per player.

Unit tests (`sys_input_tests`, new): a press and release with 0.2 ms between them reach the title as two steps; a burst
of 10 changes inside one pump arrives in order, one console frame each, double taps kept; events from two devices
interleave by timestamp; the polled fallback gives exactly today's results on today's `sys_object_tests` scenarios;
1000 events through `Push` cost under a budget (benchmark, as for every native function: docs/NATIVE_FUNCTIONS.md). In game: The Simpsons Game and
Crackdown at `frame_rate=0`, short taps in a menu and in play, plus the input script reaching the game as today.

## 2. The binding model

### Two layers (decided 2026-11-23)

Engines and platforms bind the same way: the game declares actions, devices sit underneath, a glyph is the action's
bound physical control (Steam Input's actions and action sets, Unreal's Enhanced Input actions and mapping contexts,
Unity's action maps and `GetBindingDisplayString`, Godot's `InputMap`; SDL 3 and GameInput stop at the device). An
Xbox 360 title consumes an Xbox 360 controller and turns it into actions in its own code, so Any360 binds in two
layers:

1. **The title's controller (generic, every title, always on).** Physical control -> the Xbox 360 control the title
   reads, below `XamInputGetState` / `XamInputGetKeystroke`. No title code: every title on any360::sys gets
   rebinding, keyboard and mouse, and any pad family from it. A binding's target is written `xbox:<control>`.
   Generic action names ("action_button_1") are not used: they would be the Xbox controls renamed, with no meaning
   shared across titles.
2. **The title's actions (per title, fine-tuning).** Where a title has an action table Any360 can read (Mirror's
   Edge: Unreal Engine 3's `Bindings=(Name="XboxTypeS_LeftShoulder",Command="GBA_Jump")` in its Coalesced.ini; Call
   of Duty: its binds), the title adapter declares the actions and their default controls, and a binding's target is
   `action:<name>`. The launcher then shows "Jump: LB", one action can move without the others sharing its control in
   another context (A = jump in play, confirm in menus), and the adapter writes Any360's bindings into the title's own
   table (section 4, C7). A title without one stays on layer 1.

The file takes both kinds of target from the start, so a title gaining an adapter changes no format.

### Vocabulary

- **Logical control**: what the title sees, the Xbox 360 controller: A B X Y, LB RB, LT RT (analog), LS RS (clicks),
  Start Back, D-pad up/down/left/right, left and right stick (two axes each), Guide. Titles only know these.
- **Physical control**: a control on a device, named by **position** as SDL 3 and GameInput do: face south/east/west/
  north, left/right shoulder, left/right trigger, sticks, D-pad, start/back (or options/share, plus/minus), paddles,
  touchpad click; a keyboard key (scancode, so AZERTY and QWERTY bind the same keys by place); a mouse button, wheel or
  motion.
- **Binding**: logical control <- one or more physical controls (with modifiers: a stick or trigger as a button past a
  threshold, a key as a full-tilt stick direction, mouse motion as a stick with sensitivity and curve, axis inversion,
  dead zone, stick swap).

### The file

The launcher writes one JSON file per title launch (globals merged with the title's overrides, so Any360 reads one
document) and passes it with `--any360_input_bindings=<path>` through the manifest's `options` mapping. Shape:

```json
{
  "version": 1,
  "profiles": {
    "gamepad": {
      "xbox:a": ["east"], "xbox:b": ["south"],
      "xbox:lb": ["left_trigger"], "xbox:lt": ["left_shoulder"],
      "xbox:right_y": [{"control": "right_y", "invert": true}],
      "xbox:up": ["dpad_up", "left_y+"]
    },
    "playstation": { "base": "gamepad", "xbox:back": ["touchpad"] },
    "switch": { "base": "gamepad" },
    "steam_deck": { "base": "gamepad" },
    "keyboard_mouse": {
      "xbox:a": ["key:space"], "xbox:left_y": ["key:w", {"control": "key:s", "value": -1}],
      "xbox:right_x": [{"control": "mouse:x", "sensitivity": 1.0}]
    }
  }
}
```

- **Targets** (the keys): `xbox:<control>`, the title's controller (layer 1): `a b x y lb rb lt rt ls rs start back up
  down left right left_x left_y right_x right_y` (`ls`/`rs` the stick clicks); `action:<name>` an action of the
  title's adapter (layer 2). Unknown targets are kept and reported, not fatal.
- **Sources** (the lists): physical controls by position: `south east west north left_shoulder right_shoulder
  left_trigger right_trigger left_stick right_stick dpad_up dpad_down dpad_left dpad_right start back guide
  touchpad left_x left_y right_x right_y`, a stick direction `left_x+ left_x- ...` (an axis as a button past half its
  range), keys `key:<name>` and mouse `mouse:<button|x|y|wheel>` (the names are the keyboard glyph set's,
  `KEYBOARD_NAMES` in `tools/make_glyphs.py`: `key:a`, `key:page_up`, `key:numpad_enter`, `mouse:left`, `mouse:wheel_up`...). A source is a string or an object with `control`
  and its modifiers (`invert` for an axis, `value` for a button driving an axis, `sensitivity` for the mouse).
- **How a target combines its sources**: a button is held when any source is (a trigger counts past the controller's
  threshold, a stick direction past half range); a trigger takes the largest source (a button gives full); an axis
  takes the sum of its sources, clamped (a button gives `value`, 1 by default, times full range).
- A target the file does not name keeps its own control (identity), so an empty file is today's behaviour, and a swap
  names both sides (`xbox:a` and `xbox:b`), as Steam Input's does.

- **The profile is chosen automatically, per device, per event.** Nothing in the file says which device is used: each
  event carries its device (section 1), the device's family (section 3) picks its profile, and the event is mapped
  with it. A profile falls back to its `base`, then to `gamepad` (or `keyboard_mouse`), then to the identity.
  Switching from the pad to the keyboard, from an Xbox pad to a DualSense, or plugging a new pad mid-game needs
  nothing from the player: the next event of the new device is mapped with that device's profile, and the glyphs
  follow (section 3). Two devices used at once (a pad and the mouse) are each mapped with their own profile and merged
  after the mapper, as today's merge does.
- **A player's own layout (split screen).** A profile `player<n>` (`player1` to `player4`, its `base` usually
  `gamepad`) is the pad layout of user n-1 (the slot of "Several players", section 1): the launcher writes one when a
  player's layout differs from the shared one. Built 2026-11-24: `ProfileFor(bindings, user, family)` gives user n-1
  `player<n>` when the file has it, else its pad family's profile (`playstation`...), then `gamepad`; the keyboard
  keeps `keyboard_mouse`. The controller samples (`Xam`) and the glyph query (`GlyphQuery::user(n)`) both take it, so
  a player's prompts show their own layout.
- A held input survives the switch: a button held on the pad stays held while the keyboard takes over, and its
  release, on the pad, is mapped with the pad's profile (the mapper keeps per-device state, never re-maps a held input
  with another profile).
- The keyboard/mouse profile replaces the runtime's `keybind_*` cvars when the file gives one; until then the runtime's
  driver keeps working (generic first, nothing removed before its replacement is checked in game).
- **Positions, not labels** (C2, decided). A binding names where the button is, and the default is by position on
  every pad: the south button is the title's A whatever is printed on it. On a Switch controller the south button is
  labelled "B", so it does the title's A and the prompt for A shows that controller's "B" glyph; the east button
  (labelled "A") does the title's B. Glyphs always show the label printed on the bound position (section 3), so the
  prompt and the hand agree. A player who wants the labels to match swaps A/B and X/Y in the launcher like any other
  remap.

### In any360::sys

`InputMapper` (new, `any360/sys/input.h`): built from the parsed file; `Map(InputEvent, family) -> logical changes`
with per-device held state, and the inverse `Bound(logical, family) -> physical controls` the glyph query needs. Pure functions over plain data, no
host types, so it is unit-tested alone and reused by the launcher's preview (same JSON fixtures on both sides).

Unit tests: parsing (every field, unknown fields kept, a bad entry skipped with a message, version check); identity
when empty; A/B swap both ways; one logical control from two physical ones (OR), a trigger as a button with hysteresis,
keys as a stick direction (opposite keys cancel), mouse motion scaled; profile chosen by family with the `base`
fallback chain; pad then keyboard then DualSense events interleaved, each mapped with its own profile; a button held
on the pad while the keyboard takes over, released on the pad, gives exactly one press and one release; a pad plugged
mid-run mapped from its first event; inverse map of every case; the same fixtures
parsed by the launcher's code give the same table (shared JSON golden).

## 3. The glyph API

### Families and detection (automatic)

Detection is automatic and continuous: no setting is needed, the player picks up a device and both the bindings
(section 2) and the glyphs follow it. Devices are detected when they connect (SDL's add/remove events, hot-plug
included) and the active one is re-decided at every input.

`ControllerFamily`: Xbox 360, Xbox One/Series, PlayStation 3, 4 and 5 (shapes, plus Create/Options/touchpad on 4/5),
Switch Pro and Joy-Con (lettered, A on the east), Steam Deck, Steam Controller, generic gamepad (positions only), and
keyboard and mouse.

- From SDL 3: `SDL_GetGamepadType` and `SDL_GetGamepadButtonLabel` (the label printed on a face button:
  A/B/X/Y or cross/circle/square/triangle), then vendor/product for what the type leaves open (Steam Deck: Valve
  0x28DE, product 0x1205). Behind Steam Input's virtual Xbox pad SDL reports the real controller from Steam's virtual
  gamepad info (to check on the Deck in game mode). The ReXGlue driver already logs the type, vendor and product
  (`sdl_input_driver.cpp:455`); `DeviceInfo` gains a `family`.
- Keyboard and mouse are a family of their own: a key press, a mouse click, or mouse motion past a threshold makes
  keyboard and mouse the active device, the next real pad input makes the pad active again.
- The **active device** per user is the last one that produced real input, as the runtime's `ActiveDeviceTracker`
  already does ("so button glyphs follow the pad in the player's hands"), fed by the event stream of section 1:
  neutral or drifting sticks and mouse jitter below a threshold never take over, and a change of family shown in the
  glyphs is held a short time (debounce, about 250 ms) so prompts do not flicker when both hands touch two devices.
  The bindings never wait for that debounce: each event is mapped with its own device's profile at once (section 2).
- A device unplugged hands the glyphs to the last other device used; with none left, the last family stays shown.
- Only for a pad SDL does not recognise, the launcher can pin a device's family by its GUID (`"devices": {"<guid>":
  "playstation"}`); there is no global switch to turn detection off.

### The query

```cpp
namespace any360::sys {
struct Glyph {
  ControllerFamily family;   // of the user's active device, detected
  PhysicalControl control;   // where the bound control is on that device
  GlyphId id;                // index into the glyph set: family x control
};
// The glyph for a logical control of a user's controller, through the bindings: A swapped with B gives B's glyph.
Glyph GlyphFor(uint32_t user, LogicalControl control);
// Moves on whenever an answer of GlyphFor can change (bindings reloaded, active family changed).
uint32_t GlyphGeneration();
}
```

Built (2026-11-24) as `any360/sys/input_glyphs.h`: `FamilyOf(PadDescription)`, `IsRealInput`, `ActiveDevice` per
user and `GlyphQuery`; the host owns one (`PlayerGlyph(user, control)`, players.h) fed by its devices and inputs.
An answer is `{family, set, control}`: the glyph set's name and the control's name in it (`east`, `key:space`), 2.6 ns
and no allocation. The profile chain (`ResolveProfile`: the family's profile, its bases, then `gamepad`) is shared by
the mapper and the query: each user is mapped with its pad's family profile (`Xam`, `XamHost::controller_family`), so
a glyph always shows what the input does. Drawing from the sets: the title adapters, section 4 ("Glyph pages").

When several physical controls are bound, the first one of the active device's class wins (a keyboard player sees the
key, not the pad button). The glyph images live in a **glyph set**: one atlas per family (PNG, several sizes, plus a
JSON index of rectangles per control), loaded by the host; `any360::sys` only answers ids, the host and the renderer
own the pixels. The same sets ship to the launcher, so the launcher and the game draw the same glyph for the same
control. The artwork: Any360's own pack (from Crackdown's), or the platform's glyphs where the title runs on a
platform that has them (Steam), see C3. The sets live in `assets/glyphs` (`families.json`, SVG sources, atlases
rendered by `tools/make_glyphs.py`, keyed by `PadControl` names): `xbox360`, `xbox_one`, `ps3`, `ps4`, `ps5`,
`switch`, `steam_controller`, `steam_deck` and `keyboard_mouse`.

Unit tests: family from (type, vendor, product, label) on a table of known pads; active device with drift, jitter and
a two-device hand-over; `GlyphFor` over swap, multi-bind, hot-plug, unplug, and the pad -> keyboard -> DualSense sequence (glyphs follow
after the debounce, bindings at once); every family's set has every control it
claims (index validated against the atlas size).

## 4. Getting the remapped glyph into the title's own prompts

A console title draws its prompts from its own data: button textures or atlases, characters of its own font, or tokens
in its localized strings that its text code turns into one of those. Any360 never sees "a prompt"; it sees textures,
draws, file reads and, once natively bound, the XDK's functions.

So each title gets a **title adapter**, written when the title is ported (section "Generic, per title, and
fine-tuning"): it reads how the original picks a prompt glyph and redirects that choice to `GlyphFor`. The adapter is
small and declarative where it can be (tables of codepoints, tokens, texture hashes, a function's address) and code
where it must be. Any360 provides the mechanisms adapters are built from, each generic and unit-tested once:

| | How | Generic part (Any360) | Per-title part (the adapter) | Covers |
|---|---|---|---|---|
| **A. Texture region substitution** | when the texture cache uploads a guest texture whose content hash is known, the region holding a prompt glyph is replaced in the **host** image by the glyph the query answers; guest memory is never written | `any360::gfx` texture cache: hash at upload (it already untiles and uploads on change), region overwrite, re-upload when `GlyphGeneration` moves | `titles/<title>/glyphs.json`: texture hash, rectangle, logical control (found once with a capture tool) | glyph textures, atlases, bitmap fonts whose button characters sit in a font page |
| **B. String token rewrite** | when a title's localized text reaches it through a file read, button tokens are rewritten to the bound control's token | `any360::sys` file layer: a read overlay applied to named files | the token syntax and the files | titles whose prompts are tokens in text (CoD-style) |
| **C. Declared prompt function** | the title's repository names the function that draws "button N" (address or signature); a native wrapper maps N through the inverse binding before calling it | the wrapper and the declaration format, in the binding tables | the function's address/signature and its argument | any title, exactly, once that function is found (stage 2) |
| **D. Mod hook** | a title or mod asks `GlyphFor` itself and draws | the API above | the title's own code | native ports, stage 3 |

What gets swapped differs by where the title gets the glyph:

- **A** puts the bound control's art where the title expected A's art. For an A/B swap the "A" region gets B's glyph
  and the "B" region gets A's, the title is unaware.
- **B** and **C** turn the title's request for A into a request for B, so the title draws its own B art: right for a
  swap on an Xbox pad; for another family it still needs A to show a cross or a Switch "B" (A again, or the title's own
  PlayStation art if it has some).

The survey of the test benches (section 5) adds a fifth way, **E: font glyph records** (the pad glyphs a title keeps
as characters of a font page; the title's *text* fonts are not touched, they come later), which Crackdown already uses.
There is no single first route for every title: each adapter uses the mechanism its title needs. Any360 builds a
mechanism when the first adapter needs it: **E** first (Crackdown's adapter), then **B** (Simpsons tokens), **A**
(TDU's clip bitmaps, Crackdown's XUI images), **C** where none of these reaches. **D** comes free with the API.

### Glyph pages (mechanism A by address, built 2026-11-24)

A keyed by the texture's **address**, which the adapter reads from the title's own objects (no content hash): the
title's pad glyphs are cells of texture pages (a button font's pages), and Any360 redraws each cell for its user's
bindings and device.

- `any360/sys/png.h`: PNG decoding (inflate, the five filters, 8-bit colour types) for the glyph atlases, no library.
- `any360/gfx/texture_patches.h`: a guest 2D texture shown with regions replaced in the **host** image, never in
  guest memory: the cache decodes the base level, enlarges it `scale` times (bilinear), writes the regions over it and
  builds its mips; the guest's own content still counts (a rewritten page is patched again over it). 4x by default, so
  a 32-texel cell is drawn from the 128-texel atlas: sharp, which the original art never was.
- `any360/gfx/glyph_pages.h`: the adapter's pages and cells (a cell: its rectangle, its user, a control of the title's
  controller, the bound control's glyph or its whole stick); answers through the glyph query, the art fitted in the
  cell (centred, aspect kept, alpha-weighted resampling), patches set again only when an answer changes. A cell whose
  glyph has no art (nothing of the family's kind bound) keeps the title's own.
- Host: `SetGlyphPages(pages)` (any360/host/rexglue.h), updated at each swap; the sets are copied beside the title by
  `any360_copy_glyphs(<target>)` (layer/sys/CMakeLists.txt) into `glyphs/`.

First adapter: Mirror's Edge (its repository, `src/glyph_adapter.cpp`): the MultiFont `UI_Fonts_XboxButtons` found
through UE3's GNames and GObjObjects, its characters A, B, X, Y, 0-6, L, R, H, V ([TdButtonFont] of TdGameUI.int)
mapped to the controller's controls.

Hard parts of A, to settle in its own phase:

- The hash: the texture cache's existing `content_hash` (guest bytes as stored, already computed when memory changes,
  so it costs nothing more) (C4); an adapter's table names the size and format too, so a lookup is one map probe.
- Region writes in the host image's format: DXT/BC blocks are 4x4, a glyph replaced in a compressed atlas is re-encoded
  for its blocks, or the host image is made uncompressed for these textures only.
- Fonts with button characters: same mechanism on the font page texture; character widths stay the title's, so the
  replacement glyph is fitted inside the original cell.
- Resolution: glyphs are drawn at the guest's size; at a resolution scale the region is replaced at the host image's
  size, so a 4x scale gets sharp glyphs, which the original art never had.

Unit tests for A: hash of a texture equal across endian and tiling of the same content; a region replaced in
RGBA8, DXT1, DXT5 images (compared to golden PNGs); no change to an unlisted texture; re-upload on generation change
only. In game: side-by-side captures before/after a swap and per family, and the texture
cache's frame cost with and without the table (against the original path).

## 5. The test benches

Surveyed 2026-11-14 (data and generated code of each title's repository). All four read the pad through
`XamInputGetState` on any360::sys (TDU and CoD wrap it in a lock, `src/input_lock.cpp`, but still call
`CallSystemExport(kInput)`), so the mapper of section 2 reaches every one of them with no title code.

| Title | How prompts are drawn | Swap (which button) | Family art (how it looks) |
|---|---|---|---|
| **Crackdown** | tokens in the localized strings (`<A>`, `<B>`, `<RT>`, `<DU>`... in `Local/<lang>/GameStrings.str`) turned into characters U+0190..U+01B7 of the bitmap font `Crackdown_Font.abc` + `.dds` (2048x1024 DXT3). XUI menus also use images (`XUIFrontEnd/Scene.xzp`: `BUTTON_DIGITALPAD_DOWN.png`, `controllerLayout.png`) | **already done per title** in `crackdown-any360-sys`: `src/glyph_overlay.cpp` rebuilds the font with modern glyphs, and `UpdateButtonGlyphs` (every frame) rewrites the glyph records' rectangles in guest memory from the `keybind_*` cvars (`src/button_glyph_plan.h`) | the same overlay; the XUI images are not covered |
| **The Simpsons Game** | positional tokens in localized text (`[RDDown]`, `[RDRight]`, `[LStick]`, `[Start]`... in `loc/text/*.str`, RefPack), resolved by EA APT (Flash-like UI) to a `buttons` font (`buttons.ffn`) | token rewrite on load (`[RDDown]` -> `[RDRight]`), or the font's glyph records | font page texture (region substitution), once `buttons.ffn` is decoded |
| **TDU** | mostly words ("Press the A button") or a `buttons` movie clip in `hud.bnk` chosen by the Flash variable `/:WhatButton`; `{a} {b} {lt}...` names in `DB_US.bnk` | the `/:WhatButton` letter, or the clip's bitmaps by hash; plain-word prompts cannot follow | texture substitution by hash on the clip's bitmaps |
| **CoD Black Ops** | IW bind system: `[{+usereload}]` in localized strings -> bound key -> `KEY_BUTTON_A`... -> font glyph or material (`ui_button_xenon_3d_a_32x32`); fastfiles signed, not readable yet | the game's own binds (`gpad_buttonsConfig`, presets): a remap done through binds updates prompts by itself, a host-side swap does not | the font glyph or button materials by hash |

What this changes in the plan:

- **Crackdown's overlay is the first title adapter.** It already does what an adapter does: its font's pad glyph
  records redirected per binding (swap) and their art replaced (family). The redirection becomes the generic mechanism
  **E** in Any360; what stays in Crackdown's repository is its adapter (the font object's address, the record layout,
  codepoint U+0190 + n -> logical control), now asking `GlyphFor` instead of the `keybind_*` cvars, which the bindings
  file replaces. Records are rewritten when `GlyphGeneration` moves, not every frame.
- **Two layers, kept apart.** *Which* position a prompt names (the swap) and *how* that position looks (the family)
  are two steps: swap through tokens, glyph records, a Flash variable or a declared function; art through the texture
  region of section 4 A keyed by position and family. Texture substitution alone can do both where each button has
  its own region (the region of A gets the art of `Bound(A)` in the active family).
- **Titles with their own remap (CoD) keep it, and Any360's stacks under it** (C7). Any360 maps physical controls to
  the Xbox controller the title sees; the title's own binds then map that controller to its actions. Its prompts
  already follow its binds and name an Xbox button (`KEY_BUTTON_A`), so CoD's adapter only shows the glyph of
  `Bound(that button)` on the active device: a remap made in the game, in the launcher, or both, shows right. The
  game's own remap is for the Xbox controller only; keyboard and mouse always go through Any360.

**E. Font glyph record redirection** (added from Crackdown): the title's repository declares where its button font's
glyph table lives (the font object, the offset of the character map and records, the record's rectangle fields) and
which codepoint is which logical control; Any360 rewrites the rectangles when `GlyphGeneration` moves (not every frame,
as the Crackdown overlay does today), and its glyph set provides the replacement page. Generic code, per-title data;
unit-tested on a synthetic font table in guest memory.

## 6. In the launcher

The launcher's roadmap already plans "contrôles (remappage manette et clavier, profils)" in phase 3, and its design
system has `ButtonGlyph`, `ButtonHints` and `KeyBindingTable`. What the controls module adds:

- **Shows**: one row per logical control (generic names first; a title's manifest may later name its actions, "A:
  Jump", phase 6), one tab per profile (gamepad, then PlayStation / Switch / Steam Deck when they differ from it,
  keyboard and mouse); the page **opens on the profile of the device last touched** and switches tab by itself when the
  player picks up another device, as the game does, with its glyphs; capture by pressing ("press the button for A"), conflicts (one physical control on two logical ones) shown, reset to default.
  Global values with per-title overrides, as every other option (gear icon, "Global / Personnalisé").
- **Stores**: the global and per-title bindings in its `options.json` store, and writes the merged file of section 2 next
  to the title's launch (in the launcher's data folder), passed as `--any360_input_bindings`.
- `ButtonGlyph` today is deliberately neutral (no console maker's art, "the same glyphs serve every controller"): it
  becomes family-aware by drawing from the same glyph sets as the game; the neutral style stays as the generic family.
- **Titles with an in-game remap.** The manifest declares it (`"inGameRemap": "controller"`), and the controls page
  of that title says: the game also has its own controller remap in its options (Xbox controller only); both work
  together, the game's applies on top of Any360's; keyboard and mouse are set here only.
- Big Picture: the launcher's own pad input (SDL in the shell) uses the same family detection, so the launcher's hints
  match the game's.

Tests: vitest for the module's components (capture, conflict, reset, override display); the .NET side writes the
merged file and a golden compares it; the shared JSON fixtures of section 2 parsed by both sides.

## 7. Phases

Each block gets its unit tests before it is used in game, and is turned on only when it is done.

| Phase | Block | Done when |
|---|---|---|
| 0 | Event-fed input queue (section 1): SDL event watch, `InputQueue::Push`, per-device states until the mapper | tests of section 1 green; Simpsons and Crackdown at `frame_rate=0` lose no tap; same results as today when no event source |
| 1a | The bindings file and `InputMapper` (section 2), layer 1 on the controller the host reports (the `gamepad` profile, every source a pad has), `--any360_input_bindings` | parser and mapper tests (every target and source kind, swap, trigger as button, button as axis, stick direction, invert, identity, bad entries); A/B swap felt in a menu of two titles; cost a sample measured |
| 1b | Keyboard/mouse profile, per-device profiles (needs phase 0's per-device states) | tests green; the runtime's `keybind_*` replaced only after the keyboard profile is checked in game |
| 2 | Automatic families and active device (section 3), per-family profiles chosen per event (section 2) | detection table tests; in game, with no setting: Xbox, PlayStation, Switch pad and the Steam Deck detected, hot-plug, pad -> keyboard -> DualSense mid-game with bindings and glyphs following |
| 3 | Glyph sets and `GlyphFor` (section 3) | sets complete for every family; query tests; the launcher draws from the same sets |
| 4 | Launcher controls module (section 6) | module tests; a swap made in the launcher reaches the game's input |
| 5 | Mechanism E (font glyph records) and the first title adapter, Crackdown's (its overlay on the Any360 API) | E's tests on a synthetic font table; Crackdown's pad prompts follow a swap and a device change, side by side captures; cost measured |
| 6 | The other adapters, each bringing the mechanism it needs: Simpsons (B, tokens; `buttons.ffn`), TDU (A, clip bitmaps; `/:WhatButton`), CoD (its binds, C7), Mirror's Edge (its prompts; its UE3 bindings), Crackdown's XUI images | each test bench's pad prompts follow the bindings and the device |
| 6b | Layer 2, the title's actions (section 2): adapters declaring actions read from the title's data (Mirror's Edge's `GBA_*`, CoD's binds), Any360's bindings written into the title's table, its in-game editor synced or locked (C7, C9); the launcher shows actions | an action moved alone in game; the in-game controls screen shows the launcher's binding or points to the launcher |
| 7 | Keyboard and mouse prompt glyphs (the Crackdown pack's keycaps) | prompts show keys when the keyboard is in use |
| later | Fine-tuning, closer to a remaster: text fonts, higher-resolution or redrawn art for a title's own glyphs, upscaled textures, action names in the launcher | per title |

## 8. Choices (all decided 2026-11-14)

- **C1. Event source: SDL 3 everywhere**, Windows included (one code path, also the Deck's). GameInput (Windows only)
  is not used while SDL is the backend.
- **C2. Nintendo layout default: decided (2026-11-14), by position.** The south button does the title's A on every
  pad (SDL 3's default); on a Switch pad the prompts show its printed labels (B for the title's A). Matching by label is
  an ordinary remap in the launcher, offered as one button on its Switch profile tab (A/B and X/Y swapped).
- **C3. Glyph artwork: decided (2026-11-14).** Any360's own pack, made from the one already redrawn for Crackdown
  (`crackdown-any360-sys/packaging/glyphs`: 128 px SVG sources, 40 pad glyphs, 99 keycaps, 6 mouse glyphs, no
  platform logos, built by `tools/make-button-glyphs.py`). It moves to Any360 (`assets/glyphs/`, the tool to
  `tools/`), becomes the Xbox-layout and keyboard/mouse sets, and is extended in the same style with PlayStation,
  Switch and Steam Deck sets. **On a platform that has its own glyphs, the platform's are used instead**: in the Steam
  edition, Steam Input's (`GetGlyphPNGForActionOrigin` / `GetGlyphSVGForActionOrigin` for the device's origins),
  rasterised into the same atlas layout at start and on a device change, so section 4 does not care where a glyph
  came from; Any360's pack is the fallback for a control the platform has no glyph for.
- **C4. Prompt textures are found by the texture cache's existing `content_hash`** (guest bytes as stored, already
  computed when memory changes: free). Tied to the stored layout, which is fine since an adapter's table is made from
  the title's own textures; a hash of the untiled image stays possible if a title stores one texture two ways.
  **Revised 2026-11-23: not the first choice.** A texture's bytes differ between a title's regions (EU / US) and
  languages, so a hash table is per version and per language. An adapter finds a prompt by what the title's code uses
  to choose it (a font's character, a string token, a function, an object of the title's own), read from its
  decompiled code; mechanism A keys on a hash only where nothing else is reachable. Text fonts (upscaled or redrawn)
  are fine-tuning on the same basis: Any360 provides the atlas tools, the title's adapter finds its fonts.
- **C5. Any360's bindings are the supported remap; Steam Input is left as it is** (its default layout passes the pad
  through by position). A remap the player makes in Steam is not seen by Any360, so its glyphs would not follow it;
  reading Steam's bindings through the Steam Input API is a possible later addition to the Steam edition.
- **C6. The controls page is a launcher module of its own ("Controls")**, as every launcher feature outside the core
  is, registering into the options UI (global gear and per-game page).
- **C7. Titles with their own remap (CoD): revised 2026-11-23, replaced by Any360's.** (2026-11-14 had kept both,
  stacked.) The title adapter writes Any360's bindings into the title's own table (layer 2 of section 2), where the
  title's options read them, so the in-game controls screen shows what the launcher set. Where the in-game editor
  cannot show a binding (presets only, as Black Ops' on the 360 may be: to check in its code), the adapter hides or
  replaces that option, pointing to the launcher, rather than show a wrong preset. Devices stay Any360's: the title's
  table only knows Xbox controls, keyboard and other pads go through layer 1 under it.
- **C9. One place keeps the bindings: the launcher's file (2026-11-23).** A title whose in-game editor stays usable
  gets its changes read back by its adapter into the launcher's store (two-way); otherwise the in-game editor is
  locked. Bindings never diverge between the launcher and the game.
- **C8. First in-game route: decided (2026-11-14).** No single route: a title adapter per ported title, each mechanism
  built when the first adapter needs it, Crackdown's (E) first; text fonts later, as fine-tuning.

## Open questions

- Per-title action names ("A: Jump") need each title's mapping; come from the manifest, a later phase.
- A title that reads the same button in different contexts with different meanings (A = jump and A = confirm) gets one
  binding for both: context-aware bindings are out of scope until a title needs them.
- Titles that draw prompts through vector UI (Scaleform) render glyphs from their own SWF bitmaps or fonts: A still
  applies if those end up as textures; if they are vector shapes, only C reaches them.
- XAM's own UI (message boxes, the guide) is drawn by Any360's host and uses `GlyphFor` directly.
