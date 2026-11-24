// The keyboard and mouse player (docs/GLYPHS.md section 2, "keyboard_mouse"): until Any360 maps keys itself (phase 1b),
// the bindings file's keyboard_mouse profile is handed to the runtime's own keyboard/mouse driver as its settings
// (keybind_<control> = "<Key>,<Key>", mnk_mouse for mouse look on the right stick). Pure data, no runtime call: the
// host applies the result with rex::cvar::SetFlagByName.
//
// A target the profile names replaces the runtime's default keys for it; one it does not name keeps them (the launcher
// writes every control, so its layout never shares a key with a default). Sources the runtime cannot read (the mouse
// wheel, a pad control in the keyboard profile) are reported in `problems`.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "any360/sys/input_bindings.h"

namespace any360::host::rexglue {

// A bindings file key name (`key:<name>`, `mouse:<name>`, tools/make_glyphs.py KEYBOARD_NAMES) -> the runtime's
// (rex::ui keybinds: "Space", "LBracket", "LMB"...); empty when the runtime has no such key.
inline std::string RuntimeKeyName(const sys::BindingSource& source) {
  const std::string& n = source.name;
  if (source.kind == sys::BindingSource::Kind::kMouse) {
    if (n == "left") return "LMB";
    if (n == "right") return "RMB";
    if (n == "middle") return "MMB";
    return {};
  }
  if (source.kind != sys::BindingSource::Kind::kKey || n.empty()) return {};
  if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9'))) {
    return std::string(1, char(n[0] >= 'a' ? n[0] - 'a' + 'A' : n[0]));
  }
  if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && n[1] >= '1' && n[1] <= '9') return "F" + n.substr(1);
  if (n.size() == 8 && n.starts_with("numpad_") && n[7] >= '0' && n[7] <= '9') return "Numpad" + n.substr(7);
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
      {"backtick", "Backtick"}, {"minus", "Minus"}, {"equals", "Plus"}, {"comma", "Comma"}, {"period", "Period"},
      {"semicolon", "Semicolon"}, {"slash", "Slash"}, {"backslash", "Backslash"}, {"left_bracket", "LBracket"},
      {"right_bracket", "RBracket"}, {"quote", "Quote"}, {"escape", "Escape"}, {"enter", "Return"}, {"space", "Space"},
      {"tab", "Tab"}, {"backspace", "Backspace"}, {"delete", "Delete"}, {"insert", "Insert"}, {"home", "Home"},
      {"end", "End"}, {"page_up", "PageUp"}, {"page_down", "PageDown"}, {"left", "Left"}, {"right", "Right"},
      {"up", "Up"}, {"down", "Down"}, {"shift", "Shift"}, {"control", "Control"}, {"alt", "Alt"},
      {"caps_lock", "CapsLock"}, {"numpad_enter", "NumpadEnter"}, {"numpad_plus", "NumpadPlus"},
      {"numpad_minus", "NumpadMinus"}, {"numpad_multiply", "NumpadStar"}, {"numpad_divide", "NumpadSlash"},
      {"print_screen", "PrintScreen"}, {"pause", "Pause"}, {"num_lock", "NumLock"}, {"scroll_lock", "ScrollLock"},
  };
  for (const auto& [ours, runtime] : kNames)
    if (n == ours) return std::string(runtime);
  return {};
}

struct RuntimeSetting {
  std::string name, value;
  bool operator==(const RuntimeSetting&) const = default;
};

// The runtime settings for `bindings`' keyboard_mouse profile (its base chain resolved): empty when it has none.
inline std::vector<RuntimeSetting> RuntimeKeybinds(const sys::InputBindings& bindings,
                                                   std::vector<std::string>* problems = nullptr) {
  std::vector<RuntimeSetting> out;
  if (!bindings.Find("keyboard_mouse")) return out;
  const auto resolved = sys::ResolveProfile(bindings, "keyboard_mouse");
  using X = sys::XboxControl;
  // Buttons: one setting each. Axes: a setting for each direction, a source's sign (its `value`, inverted by `invert`)
  // picking which.
  struct Button {
    X control;
    const char* setting;
  };
  static constexpr Button kButtons[] = {
      {X::kA, "keybind_a"}, {X::kB, "keybind_b"}, {X::kX, "keybind_x"}, {X::kY, "keybind_y"},
      {X::kLb, "keybind_left_shoulder"}, {X::kRb, "keybind_right_shoulder"}, {X::kLt, "keybind_left_trigger"},
      {X::kRt, "keybind_right_trigger"}, {X::kLs, "keybind_lstick_press"}, {X::kRs, "keybind_rstick_press"},
      {X::kStart, "keybind_start"}, {X::kBack, "keybind_back"}, {X::kUp, "keybind_dpad_up"},
      {X::kDown, "keybind_dpad_down"}, {X::kLeft, "keybind_dpad_left"}, {X::kRight, "keybind_dpad_right"},
  };
  struct Axis {
    X control;
    const char *plus, *minus;
  };
  static constexpr Axis kAxes[] = {
      {X::kLeftX, "keybind_lstick_right", "keybind_lstick_left"},
      {X::kLeftY, "keybind_lstick_up", "keybind_lstick_down"},
      {X::kRightX, "keybind_rstick_right", "keybind_rstick_left"},
      {X::kRightY, "keybind_rstick_up", "keybind_rstick_down"},
  };
  auto append = [](std::string& list, const std::string& key) {
    if (key.empty()) return;
    list += (list.empty() ? "" : ",") + key;
  };
  auto report = [&](const sys::Binding& b, const sys::BindingSource& s) {
    if (problems)
      problems->push_back(b.target + ": " + (s.kind == sys::BindingSource::Kind::kPad ? "a pad control" : s.name) +
                          " is not a key or mouse button the runtime's keyboard driver reads");
  };
  for (const Button& button : kButtons) {
    const sys::Binding* b = resolved[size_t(button.control)];
    if (!b) continue;
    std::string keys;
    for (const sys::BindingSource& s : b->sources) {
      const std::string key = RuntimeKeyName(s);
      if (key.empty()) report(*b, s);
      append(keys, key);
    }
    out.push_back({button.setting, keys});
  }
  bool mouse_look = false;
  double sensitivity = 0;
  for (const Axis& axis : kAxes) {
    const sys::Binding* b = resolved[size_t(axis.control)];
    if (!b) continue;
    std::string plus, minus;
    for (const sys::BindingSource& s : b->sources) {
      const bool right_stick = axis.control == X::kRightX || axis.control == X::kRightY;
      if (s.kind == sys::BindingSource::Kind::kMouse && (s.name == "x" || s.name == "y") && right_stick) {
        mouse_look = true;
        sensitivity = std::max(sensitivity, double(s.sensitivity));
        continue;
      }
      const std::string key = RuntimeKeyName(s);
      if (key.empty()) {
        report(*b, s);
        continue;
      }
      append((s.value < 0) != s.invert ? minus : plus, key);
    }
    out.push_back({axis.plus, plus});
    out.push_back({axis.minus, minus});
  }
  out.push_back({"mnk_mouse", mouse_look ? "true" : "false"});
  if (mouse_look && sensitivity > 0) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", sensitivity);
    out.push_back({"mnk_sensitivity", text});
  }
  return out;
}

}  // namespace any360::host::rexglue
