// The keyboard and mouse player's layout handed to the runtime's keyboard driver (any360/host/keyboard_binds.h): every
// key name of the bindings file the runtime knows, buttons and stick directions to their keybind_* settings, mouse
// look to mnk_mouse, what the runtime cannot read reported.
#include <cstdio>
#include <string>
#include <vector>

#include "any360/host/keyboard_binds.h"

using namespace any360::host::rexglue;
using any360::sys::BindingSource;
using any360::sys::InputBindings;

namespace {
int g_checks = 0, g_failures = 0;
#define CHECK(condition)                                                                                  \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    if (!(condition) && ++g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  } while (0)

std::string Value(const std::vector<RuntimeSetting>& settings, const std::string& name) {
  for (const RuntimeSetting& s : settings)
    if (s.name == name) return s.value;
  return "<none>";
}

std::string Key(const char* name) {
  BindingSource s;
  s.kind = BindingSource::Kind::kKey;
  s.name = name;
  return RuntimeKeyName(s);
}
}  // namespace

int main() {
  // Key names: letters, digits, function keys, numpad, the named keys, mouse buttons; unknown ones and the wheel empty.
  CHECK(Key("a") == "A" && Key("z") == "Z" && Key("0") == "0" && Key("9") == "9");
  CHECK(Key("f1") == "F1" && Key("f12") == "F12" && Key("numpad_7") == "Numpad7");
  CHECK(Key("space") == "Space" && Key("enter") == "Return" && Key("left_bracket") == "LBracket" &&
        Key("page_down") == "PageDown" && Key("equals") == "Plus" && Key("numpad_multiply") == "NumpadStar" &&
        Key("caps_lock") == "CapsLock" && Key("shift") == "Shift");
  CHECK(Key("nope").empty() && Key("").empty() && Key("f0").empty() && Key("fx").empty());
  BindingSource mouse;
  mouse.kind = BindingSource::Kind::kMouse;
  mouse.name = "left";
  CHECK(RuntimeKeyName(mouse) == "LMB");
  mouse.name = "wheel_up";
  CHECK(RuntimeKeyName(mouse).empty());

  // No keyboard profile: nothing changes.
  CHECK(RuntimeKeybinds(InputBindings::Parse(R"({"version":1,"profiles":{"gamepad":{"xbox:a":["east"]}}})")).empty());

  // The launcher's layout: buttons, both sticks from keys, mouse look, triggers on the mouse buttons.
  std::vector<std::string> problems;
  const auto settings = RuntimeKeybinds(InputBindings::Parse(R"({"version":1,"profiles":{"keyboard_mouse":{
    "xbox:a":["key:space"], "xbox:b":["key:backspace","key:c"], "xbox:lt":["mouse:right"], "xbox:rt":["mouse:left"],
    "xbox:up":["key:1"], "xbox:start":["key:enter"],
    "xbox:left_x":[{"control":"key:a","value":-1},"key:d"], "xbox:left_y":["key:w",{"control":"key:s","value":-1}],
    "xbox:right_x":["key:right",{"control":"key:left","value":-1},{"control":"mouse:x","sensitivity":1.5}],
    "xbox:right_y":[{"control":"key:down","invert":true},"key:up",{"control":"mouse:y","sensitivity":1.5}],
    "xbox:x":["mouse:wheel_up"]}}})"), &problems);
  CHECK(Value(settings, "keybind_a") == "Space");
  CHECK(Value(settings, "keybind_b") == "Backspace,C");
  CHECK(Value(settings, "keybind_left_trigger") == "RMB" && Value(settings, "keybind_right_trigger") == "LMB");
  CHECK(Value(settings, "keybind_dpad_up") == "1" && Value(settings, "keybind_start") == "Return");
  CHECK(Value(settings, "keybind_lstick_left") == "A" && Value(settings, "keybind_lstick_right") == "D");
  CHECK(Value(settings, "keybind_lstick_up") == "W" && Value(settings, "keybind_lstick_down") == "S");
  CHECK(Value(settings, "keybind_rstick_right") == "Right" && Value(settings, "keybind_rstick_left") == "Left");
  CHECK(Value(settings, "keybind_rstick_down") == "Down" && Value(settings, "keybind_rstick_up") == "Up");
  CHECK(Value(settings, "mnk_mouse") == "true" && Value(settings, "mnk_sensitivity") == "1.5");
  // The wheel cannot be read by the runtime: X gets no key, and says so.
  CHECK(Value(settings, "keybind_x") == "" && problems.size() == 1 && problems[0].starts_with("xbox:x"));
  // A control the profile does not name keeps the runtime's default (no setting for it).
  CHECK(Value(settings, "keybind_y") == "<none>" && Value(settings, "keybind_dpad_down") == "<none>");

  // Without mouse look, it is switched off; a base profile's entries count.
  const auto keys_only = RuntimeKeybinds(InputBindings::Parse(R"({"version":1,"profiles":{
    "shared":{"xbox:y":["key:p"]}, "keyboard_mouse":{"base":"shared","xbox:a":["key:space"]}}})"));
  CHECK(Value(keys_only, "mnk_mouse") == "false" && Value(keys_only, "keybind_y") == "P");

  // With no file, the driver gets Any360's layout (WithDefaultKeyboard), every control.
  const auto any360_layout = RuntimeKeybinds(InputBindings{}.WithDefaultKeyboard());
  CHECK(Value(any360_layout, "keybind_a") == "Space" && Value(any360_layout, "keybind_b") == "Control");
  CHECK(Value(any360_layout, "keybind_left_trigger") == "RMB" && Value(any360_layout, "keybind_right_trigger") == "LMB");
  CHECK(Value(any360_layout, "keybind_start") == "Escape" && Value(any360_layout, "keybind_back") == "Tab");
  CHECK(Value(any360_layout, "keybind_dpad_up") == "1" && Value(any360_layout, "keybind_dpad_left") == "4");
  CHECK(Value(any360_layout, "keybind_lstick_up") == "W" && Value(any360_layout, "keybind_lstick_left") == "A");
  CHECK(Value(any360_layout, "mnk_mouse") == "true");
  std::printf("host_keyboard_tests: %d checks, %d failing checks\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
