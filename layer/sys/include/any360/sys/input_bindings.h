// Input bindings (docs/GLYPHS.md, section 2): the file Any360Launcher writes (--any360_input_bindings) and the mapper
// that applies its first layer, physical controls -> the Xbox 360 controller the title reads, below XamInputGetState
// and XamInputGetKeystroke. Plain data and pure functions, no host types: unit-tested alone, and read the same way by
// the launcher's preview.
//
//   {"version": 1, "profiles": {"gamepad": {"xbox:a": ["east"], "xbox:b": ["south"],
//                                           "xbox:right_y": [{"control": "right_y", "invert": true}]}}}
//
// A target the profile does not name keeps its own control, so an empty file changes nothing. Targets `action:<name>`
// (a title's actions, layer 2) are parsed and kept for the title's adapter; the mapper leaves them alone.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "any360/sys/input.h"

namespace any360::sys {

// The title's controller: what a target `xbox:<name>` names.
enum class XboxControl : uint8_t {
  kA, kB, kX, kY, kLb, kRb, kLt, kRt, kLs, kRs, kStart, kBack, kUp, kDown, kLeft, kRight,
  kLeftX, kLeftY, kRightX, kRightY, kCount
};
// A pad's physical controls by position (SDL 3's naming), and each stick axis's two directions as buttons.
enum class PadControl : uint8_t {
  kSouth, kEast, kWest, kNorth, kLeftShoulder, kRightShoulder, kLeftTrigger, kRightTrigger, kLeftStick, kRightStick,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kStart, kBack, kGuide, kTouchpad,
  kLeftX, kLeftY, kRightX, kRightY,
  kLeftXPlus, kLeftXMinus, kLeftYPlus, kLeftYMinus, kRightXPlus, kRightXMinus, kRightYPlus, kRightYMinus,
  kCount
};

std::string_view Name(XboxControl c);
std::string_view Name(PadControl c);
std::optional<XboxControl> ParseXboxControl(std::string_view name);
std::optional<PadControl> ParsePadControl(std::string_view name);

struct BindingSource {
  enum class Kind : uint8_t { kPad, kKey, kMouse };
  Kind kind = Kind::kPad;
  PadControl pad = PadControl::kSouth;
  std::string name;          // a key's or the mouse's control (`key:space` -> "space"); empty for a pad control
  bool invert = false;       // an axis source, reversed
  float value = 1.0f;        // a button driving an axis: the share of the full range (-1 to 1)
  float sensitivity = 1.0f;  // the mouse
};

struct Binding {
  std::string target;                // as written: "xbox:a", "action:GBA_Jump"
  std::optional<XboxControl> xbox;   // set for an `xbox:` target
  std::vector<BindingSource> sources;
};

struct BindingProfile {
  std::string name, base;
  std::vector<Binding> bindings;
};

class InputBindings {
 public:
  // Parses the launcher's file. Entries it cannot read are skipped and described in `problems`; a file of another
  // version, or not a bindings document, gives no profiles (and says so).
  static InputBindings Parse(std::string_view text, std::vector<std::string>* problems = nullptr);
  const BindingProfile* Find(std::string_view profile) const;
  const std::vector<BindingProfile>& profiles() const { return profiles_; }

 private:
  std::vector<BindingProfile> profiles_;
};

// Layer 1 for one controller: the physical state the host reports (a pad by position, as SDL 3 lays it out: south is
// the Xbox A's place) -> the state the title sees. A profile's `base` chain is resolved when the mapper is made; a
// sample costs a pass over the named targets, no allocation.
class InputMapper {
 public:
  InputMapper() = default;  // the identity
  InputMapper(const InputBindings& bindings, std::string_view profile);

  bool identity() const { return identity_; }
  GamepadState Map(const GamepadState& physical);
  // The pad controls bound to a target, in the file's order (the glyph query's inverse): its own control when the
  // profile does not name it.
  std::vector<PadControl> Bound(XboxControl target) const;

  // A trigger counts as pressed past these (XINPUT_GAMEPAD_TRIGGER_THRESHOLD), a stick direction past half range;
  // each stays pressed until a little below, as the input queue's digital inputs do.
  static constexpr int kTriggerOn = 30, kTriggerOff = 20, kStickOn = 0x4000, kStickOff = 0x3000;

 private:
  struct Source {
    PadControl pad;
    bool invert;
    float value;
  };
  struct Target {
    bool named = false;
    uint16_t first = 0, count = 0;  // into sources_
  };
  std::array<Target, size_t(XboxControl::kCount)> targets_{};
  std::vector<Source> sources_;
  std::vector<uint8_t> held_;  // per source: pressed as a button (hysteresis)
  bool identity_ = true;
};

}  // namespace any360::sys
