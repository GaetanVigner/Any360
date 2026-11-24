// Several players on the host (docs/GLYPHS.md section 1, "Several players"; section 3): any360::sys's PlayerSlots
// decides which of the runtime's input devices feeds which user, through the runtime's own DeviceAssignment hook, so
// split-screen players keep their places; each pad's family from its SDL GUID and name (sys::DescribePad: no SDL call, the title's SDL is not the runtime's), the active device of
// each player from the states the runtime reads, and the glyph query answers from them.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "any360/sys/input.h"
#include "any360/sys/input_bindings.h"
#include "any360/sys/input_glyphs.h"

namespace rex::input {
class InputSystem;
}

namespace any360::host::rexglue {

// Installs Any360's assignment on `input` (once; the caller holds LockHostInput). `log` reports players joining and
// leaving and their families.
void InstallPlayers(rex::input::InputSystem* input, std::function<void(const std::string&)> log);
bool PlayersInstalled();
// After the runtime read `merged` (every device of `user` folded): which of the user's devices gave real input, for
// the glyphs. Reads the user's pad alone through `input` when the keyboard shares the user (the caller holds
// LockHostInput).
void ObservePlayer(rex::input::InputSystem* input, uint32_t user, const sys::GamepadState& merged);
// The family of the pad `user` plays on (kCount when none or unknown): the bindings profile follows it.
sys::ControllerFamily PlayerFamily(uint32_t user);
// The glyph of a control of the title's controller for `user`, now (docs/GLYPHS.md section 3).
sys::Glyph PlayerGlyph(uint32_t user, sys::XboxControl control);
// The bindings the glyphs follow (the same file as the input's).
void SetPlayerBindings(const sys::InputBindings& bindings);

}  // namespace any360::host::rexglue
