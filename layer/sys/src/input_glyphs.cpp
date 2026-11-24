#include "any360/sys/input_glyphs.h"

#include <cstdlib>

namespace any360::sys {

namespace {

constexpr std::string_view kFamilyNames[] = {"xbox360", "xbox_one",   "ps3",          "ps4",     "ps5",
                                             "switch",  "steam_controller", "steam_deck", "generic", "keyboard_mouse"};
static_assert(std::size(kFamilyNames) == size_t(ControllerFamily::kCount));

constexpr uint16_t kValve = 0x28DE;
constexpr uint16_t kSteamDeck = 0x1205;
constexpr uint16_t kSteamControllers[] = {0x1102, 0x1106, 0x1142};  // wired, Bluetooth, wireless receiver

// Whether a family's pad has a control (only the PlayStation 4 and 5 have a touchpad).
bool Has(ControllerFamily f, PadControl c) {
  return c != PadControl::kTouchpad || f == ControllerFamily::kPs4 || f == ControllerFamily::kPs5;
}

// Pushed past `threshold` either way.
bool Past(int v, int threshold) { return std::abs(v) > threshold; }

}  // namespace

std::string_view Name(ControllerFamily f) { return f < ControllerFamily::kCount ? kFamilyNames[size_t(f)] : ""; }

std::string_view GlyphSet(ControllerFamily f) {
  return f == ControllerFamily::kGeneric ? Name(ControllerFamily::kXboxOne) : Name(f);
}

std::string_view ProfileName(ControllerFamily f) {
  switch (f) {
    case ControllerFamily::kPs3:
    case ControllerFamily::kPs4:
    case ControllerFamily::kPs5: return "playstation";
    case ControllerFamily::kSwitch: return "switch";
    case ControllerFamily::kSteamDeck: return "steam_deck";
    case ControllerFamily::kKeyboardMouse: return "keyboard_mouse";
    default: return "gamepad";
  }
}

ControllerFamily FamilyOf(const PadDescription& pad) {
  if (pad.vendor == kValve) {
    if (pad.product == kSteamDeck) return ControllerFamily::kSteamDeck;
    for (uint16_t p : kSteamControllers)
      if (pad.product == p) return ControllerFamily::kSteamController;
  }
  switch (pad.type) {
    case PadType::kXbox360: return ControllerFamily::kXbox360;
    case PadType::kXboxOne: return ControllerFamily::kXboxOne;
    case PadType::kPs3: return ControllerFamily::kPs3;
    case PadType::kPs4: return ControllerFamily::kPs4;
    case PadType::kPs5: return ControllerFamily::kPs5;
    case PadType::kSwitchPro:
    case PadType::kJoyConLeft:
    case PadType::kJoyConRight:
    case PadType::kJoyConPair: return ControllerFamily::kSwitch;
    default: break;
  }
  switch (pad.south_label) {
    case FaceLabel::kCross: return ControllerFamily::kPs4;
    case FaceLabel::kB: return ControllerFamily::kSwitch;
    case FaceLabel::kA: return ControllerFamily::kXboxOne;
    default: return ControllerFamily::kGeneric;
  }
}

bool IsRealInput(const GamepadState& before, const GamepadState& after) {
  if (before.buttons != after.buttons) return true;
  const int trigger = InputMapper::kTriggerOn, stick = InputMapper::kStickOn;
  if ((before.left_trigger > trigger) != (after.left_trigger > trigger)) return true;
  if ((before.right_trigger > trigger) != (after.right_trigger > trigger)) return true;
  // A stick crossing half range, or flipping side while past it.
  const int16_t b[] = {before.thumb_lx, before.thumb_ly, before.thumb_rx, before.thumb_ry};
  const int16_t a[] = {after.thumb_lx, after.thumb_ly, after.thumb_rx, after.thumb_ry};
  for (int i = 0; i < 4; ++i) {
    const bool was = Past(b[i], stick), is = Past(a[i], stick);
    if (was != is || (is && (a[i] > 0) != (b[i] > 0))) return true;
  }
  return false;
}

ActiveDevice::Device* ActiveDevice::Find(uint32_t id) {
  for (size_t i = 0; i < count_; ++i)
    if (devices_[i].id == id) return &devices_[i];
  return nullptr;
}

void ActiveDevice::Show(ControllerFamily f) {
  if (f != shown_) {
    shown_ = f;
    ++generation_;
  }
  any_shown_ = true;
  pending_ = false;
}

void ActiveDevice::Connected(uint32_t device, ControllerFamily family) {
  if (Device* d = Find(device)) {
    d->family = family;
  } else if (count_ < kMaxDevices) {
    devices_[count_++] = {device, family, false, {}};
  } else {
    return;
  }
  if (!any_shown_) Show(family);
}

void ActiveDevice::Disconnected(uint32_t device) {
  Device* d = Find(device);
  if (!d) return;
  *d = devices_[--count_];
  if (count_ == 0) return;  // the last family stays shown
  // The last other device used, else the first connected.
  const Device* next = &devices_[0];
  for (size_t i = 0; i < count_; ++i)
    if (devices_[i].used && (!next->used || devices_[i].last_input > next->last_input)) next = &devices_[i];
  Show(next->family);
}

void ActiveDevice::Input(uint32_t device, Clock::time_point at) {
  Device* d = Find(device);
  if (!d) return;
  d->used = true;
  d->last_input = at;
  if (!any_shown_) {
    Show(d->family);
  } else if (d->family == shown_) {
    pending_ = false;  // back on the shown family: no change
  } else if (!pending_ || candidate_ != d->family) {
    candidate_ = d->family;
    candidate_since_ = at;
    pending_ = true;
  }
}

ControllerFamily ActiveDevice::Shown(Clock::time_point at) {
  if (pending_ && at - candidate_since_ >= kDebounce) Show(candidate_);
  return shown_;
}

void GlyphQuery::SetBindings(const InputBindings& bindings) {
  for (size_t f = 0; f < size_t(ControllerFamily::kCount); ++f) {
    const bool keyboard = ControllerFamily(f) == ControllerFamily::kKeyboardMouse;
    const auto chosen = ResolveProfile(bindings, ProfileName(ControllerFamily(f)), keyboard ? "" : "gamepad");
    for (size_t t = 0; t < size_t(XboxControl::kCount); ++t) {
      std::string& name = names_[f][t];
      name.clear();
      if (!chosen[t]) {
        // Unnamed: its own control on a pad, nothing on the keyboard.
        if (!keyboard) name = Name(OwnPlace(XboxControl(t)));
        continue;
      }
      for (const BindingSource& s : chosen[t]->sources) {
        if (keyboard && s.kind == BindingSource::Kind::kKey) name = "key:" + s.name;
        else if (keyboard && s.kind == BindingSource::Kind::kMouse) name = "mouse:" + s.name;
        else if (!keyboard && s.kind == BindingSource::Kind::kPad && Has(ControllerFamily(f), s.pad)) name = Name(s.pad);
        if (!name.empty()) break;
      }
    }
  }
  ++bindings_generation_;
}

Glyph GlyphQuery::For(uint32_t user_index, XboxControl control, Clock::time_point at) {
  const ControllerFamily f = user(user_index).Shown(at);
  Glyph g;
  g.family = f;
  g.set = GlyphSet(f);
  if (control < XboxControl::kCount) g.control = names_[size_t(f)][size_t(control)];
  return g;
}

uint32_t GlyphQuery::Generation(Clock::time_point at) {
  uint32_t generation = bindings_generation_;
  for (ActiveDevice& u : users_) {
    u.Shown(at);
    generation += u.generation();
  }
  return generation;
}

}  // namespace any360::sys
