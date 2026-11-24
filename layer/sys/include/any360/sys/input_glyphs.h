// The glyph query (docs/GLYPHS.md, section 3): which glyph shows a control of the title's controller, through the
// bindings, on the device in the player's hands. Plain data and pure functions, no host types: the host describes its
// pads (SDL 3's gamepad type, vendor, product, the south button's label) and reports real inputs; any360::sys answers
// a glyph set and a control name, the host and the renderer own the pixels (assets/glyphs/<set>.json, <set>_<size>.png).
//
//   GlyphQuery glyphs(bindings);
//   glyphs.user(0).Connected(device, FamilyOf(description));
//   glyphs.user(0).Input(device, now);                    // on each real input (IsRealInput)
//   const Glyph g = glyphs.For(0, XboxControl::kA, now);  // {kPs5, "ps5", "east"} with A bound to east on a DualSense
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "any360/sys/input.h"
#include "any360/sys/input_bindings.h"

namespace any360::sys {

enum class ControllerFamily : uint8_t {
  kXbox360, kXboxOne, kPs3, kPs4, kPs5, kSwitch, kSteamController, kSteamDeck, kGeneric, kKeyboardMouse, kCount
};
// "xbox360", "xbox_one", "ps3", "ps4", "ps5", "switch", "steam_controller", "steam_deck", "generic", "keyboard_mouse".
std::string_view Name(ControllerFamily f);
// The glyph set a family is drawn with (assets/glyphs/<set>.json): its own name; a generic pad by position, with the
// Xbox One set.
std::string_view GlyphSet(ControllerFamily f);
// The bindings profile a family's events are mapped with, before the `base` chain: "playstation", "switch",
// "steam_deck", "keyboard_mouse", "gamepad" for the others.
std::string_view ProfileName(ControllerFamily f);

// A pad as the host sees it. `type` follows SDL 3's SDL_GamepadType, `south_label` SDL_GetGamepadButtonLabel of the
// south button; the host converts, any360::sys never includes SDL.
enum class PadType : uint8_t {
  kUnknown, kStandard, kXbox360, kXboxOne, kPs3, kPs4, kPs5, kSwitchPro, kJoyConLeft, kJoyConRight, kJoyConPair
};
enum class FaceLabel : uint8_t { kUnknown, kA, kB, kX, kY, kCross, kCircle, kSquare, kTriangle };
struct PadDescription {
  PadType type = PadType::kUnknown;
  uint16_t vendor = 0, product = 0;
  FaceLabel south_label = FaceLabel::kUnknown;
};
// Valve's Steam Deck and Steam Controller by vendor and product first (SDL types them as Xbox pads), then the type,
// then, for a pad SDL does not know, the south button's label (cross: PlayStation 4; B: Nintendo layout; A: Xbox
// One); otherwise generic.
ControllerFamily FamilyOf(const PadDescription& pad);

// Whether a pad's change from `before` to `after` is a player's input, not noise: a button changed, a trigger crossed
// its threshold, a stick crossed half its range (InputMapper's thresholds). A drifting or resting stick never counts.
bool IsRealInput(const GamepadState& before, const GamepadState& after);

// One user's devices and which one's glyphs show. The active device is the last one that gave real input; a change of
// family shows once the new family has stayed active for kDebounce, so prompts do not flicker when both hands touch
// two devices. A device unplugged hands the glyphs over at once to the last other device used (or, if none was used,
// to another connected one); with none left the last family stays shown.
class ActiveDevice {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr Clock::duration kDebounce = std::chrono::milliseconds(250);
  static constexpr size_t kMaxDevices = 16;

  void Connected(uint32_t device, ControllerFamily family);
  void Disconnected(uint32_t device);
  void Input(uint32_t device, Clock::time_point at);
  // The family whose glyphs show at `at`; the title's own (Xbox 360) before any device.
  ControllerFamily Shown(Clock::time_point at);
  // Moves on each change of Shown.
  uint32_t generation() const { return generation_; }

 private:
  struct Device {
    uint32_t id;
    ControllerFamily family;
    bool used;
    Clock::time_point last_input;
  };
  Device* Find(uint32_t id);
  void Show(ControllerFamily f);

  std::array<Device, kMaxDevices> devices_{};
  size_t count_ = 0;
  ControllerFamily shown_ = ControllerFamily::kXbox360;
  bool any_shown_ = false;                 // a device's family was shown (else the title's default)
  ControllerFamily candidate_ = ControllerFamily::kXbox360;
  bool pending_ = false;                   // candidate_ differs from shown_ since candidate_since_
  Clock::time_point candidate_since_{};
  uint32_t generation_ = 0;
};

struct Glyph {
  ControllerFamily family = ControllerFamily::kXbox360;
  std::string_view set;      // the glyph set to draw from
  std::string_view control;  // the glyph's name in the set ("east", "left_y+", "key:space"); empty when nothing of the
                             // family's kind is bound to the control (a keyboard with no key for it)
};

// The glyph query for up to kUsers users. Answers are valid until the next SetBindings (Generation moves).
class GlyphQuery {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t kUsers = 4;

  GlyphQuery() { SetBindings(InputBindings{}); }
  explicit GlyphQuery(const InputBindings& bindings) { SetBindings(bindings); }
  // Resolves every family's profile into its table; the glyphs then follow the new bindings at once.
  void SetBindings(const InputBindings& bindings);
  ActiveDevice& user(uint32_t user) { return users_[user < kUsers ? user : 0]; }

  // The glyph of `control` for `user` at `at`: the shown family's glyph of the first source bound to it of that
  // family's kind (a pad control the family's pad has, a key or mouse control for the keyboard); its own place when
  // unbound on a pad. No allocation.
  Glyph For(uint32_t user, XboxControl control, Clock::time_point at);
  // Moves on whenever an answer of For can change (bindings set, a user's shown family changed).
  uint32_t Generation(Clock::time_point at);

 private:
  std::array<std::array<std::string, size_t(XboxControl::kCount)>, size_t(ControllerFamily::kCount)> names_;
  std::array<ActiveDevice, kUsers> users_{};
  uint32_t bindings_generation_ = 0;
};

}  // namespace any360::sys
