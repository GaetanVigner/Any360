#include "any360/sys/input_bindings.h"

#include <algorithm>
#include <cmath>

#include "any360/sys/json.h"

namespace any360::sys {

namespace {

constexpr std::string_view kXboxNames[] = {"a",  "b",     "x",    "y",  "lb",   "rb",    "lt",     "rt",     "ls",     "rs",
                                           "start", "back", "up", "down", "left", "right", "left_x", "left_y", "right_x", "right_y"};
static_assert(std::size(kXboxNames) == size_t(XboxControl::kCount));

constexpr std::string_view kPadNames[] = {
    "south",    "east",     "west",       "north",      "left_shoulder", "right_shoulder", "left_trigger", "right_trigger",
    "left_stick", "right_stick", "dpad_up", "dpad_down", "dpad_left",    "dpad_right",     "start",        "back",
    "guide",    "touchpad", "left_x",     "left_y",     "right_x",       "right_y",        "left_x+",      "left_x-",
    "left_y+",  "left_y-",  "right_x+",   "right_x-",   "right_y+",      "right_y-"};
static_assert(std::size(kPadNames) == size_t(PadControl::kCount));

// The pad control in each Xbox control's place (the identity binding), and the button bit of the digital ones.
constexpr PadControl kOwnPlace[] = {
    PadControl::kSouth,      PadControl::kEast,         PadControl::kWest,        PadControl::kNorth,
    PadControl::kLeftShoulder, PadControl::kRightShoulder, PadControl::kLeftTrigger, PadControl::kRightTrigger,
    PadControl::kLeftStick,  PadControl::kRightStick,   PadControl::kStart,       PadControl::kBack,
    PadControl::kDpadUp,     PadControl::kDpadDown,     PadControl::kDpadLeft,    PadControl::kDpadRight,
    PadControl::kLeftX,      PadControl::kLeftY,        PadControl::kRightX,      PadControl::kRightY};
static_assert(std::size(kOwnPlace) == size_t(XboxControl::kCount));

constexpr uint16_t kXboxBit[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0,      0,      0x0040, 0x0080,
                                 0x0010, 0x0020, 0x0001, 0x0002, 0x0004, 0x0008, 0,      0,      0,      0};

// The button bit of a pad control the host reports (by position, so the bit of the Xbox control in its place).
uint16_t PadBit(PadControl c) {
  switch (c) {
    case PadControl::kSouth: return 0x1000;
    case PadControl::kEast: return 0x2000;
    case PadControl::kWest: return 0x4000;
    case PadControl::kNorth: return 0x8000;
    case PadControl::kLeftShoulder: return 0x0100;
    case PadControl::kRightShoulder: return 0x0200;
    case PadControl::kLeftStick: return 0x0040;
    case PadControl::kRightStick: return 0x0080;
    case PadControl::kDpadUp: return 0x0001;
    case PadControl::kDpadDown: return 0x0002;
    case PadControl::kDpadLeft: return 0x0004;
    case PadControl::kDpadRight: return 0x0008;
    case PadControl::kStart: return 0x0010;
    case PadControl::kBack: return 0x0020;
    default: return 0;  // the guide and touchpad: the host's controller state does not report them
  }
}

bool IsTrigger(PadControl c) { return c == PadControl::kLeftTrigger || c == PadControl::kRightTrigger; }
bool IsAxis(PadControl c) { return c >= PadControl::kLeftX && c <= PadControl::kRightY; }
bool IsDirection(PadControl c) { return c >= PadControl::kLeftXPlus; }

int Axis(const GamepadState& s, PadControl c) {
  switch (c) {
    case PadControl::kLeftX: case PadControl::kLeftXPlus: case PadControl::kLeftXMinus: return s.thumb_lx;
    case PadControl::kLeftY: case PadControl::kLeftYPlus: case PadControl::kLeftYMinus: return s.thumb_ly;
    case PadControl::kRightX: case PadControl::kRightXPlus: case PadControl::kRightXMinus: return s.thumb_rx;
    default: return s.thumb_ry;
  }
}
// A stick direction's travel (0 to 32767, or 32768 for the minimum), a raw axis's distance from the centre.
int Travel(const GamepadState& s, PadControl c) {
  const int v = Axis(s, c);
  if (!IsDirection(c)) return std::abs(v);
  const bool plus = (int(c) - int(PadControl::kLeftXPlus)) % 2 == 0;
  return plus ? std::max(v, 0) : std::max(-v, 0);
}

bool IsXboxAxis(XboxControl c) { return c >= XboxControl::kLeftX; }
bool IsXboxTrigger(XboxControl c) { return c == XboxControl::kLt || c == XboxControl::kRt; }

void Problem(std::vector<std::string>* problems, std::string text) {
  if (problems) problems->push_back(std::move(text));
}

std::optional<BindingSource> ParseSource(const json::Value& v, std::vector<std::string>* problems, const std::string& where) {
  const json::Value* control = v.is(json::Value::Type::kString) ? &v : v.Find("control");
  if (!control || !control->is(json::Value::Type::kString)) {
    Problem(problems, where + ": a source is a control name or an object with \"control\"");
    return std::nullopt;
  }
  BindingSource s;
  const std::string& name = control->string;
  if (name.starts_with("key:") && name.size() > 4) {
    s.kind = BindingSource::Kind::kKey;
    s.name = name.substr(4);
  } else if (name.starts_with("mouse:") && name.size() > 6) {
    s.kind = BindingSource::Kind::kMouse;
    s.name = name.substr(6);
  } else if (const auto pad = ParsePadControl(name)) {
    s.pad = *pad;
  } else {
    Problem(problems, where + ": unknown control \"" + name + "\"");
    return std::nullopt;
  }
  if (v.is(json::Value::Type::kObject)) {
    if (const json::Value* invert = v.Find("invert"); invert && invert->is(json::Value::Type::kBool)) s.invert = invert->boolean;
    if (const json::Value* value = v.Find("value"); value && value->is(json::Value::Type::kNumber))
      s.value = float(std::clamp(value->number, -1.0, 1.0));
    if (const json::Value* sens = v.Find("sensitivity"); sens && sens->is(json::Value::Type::kNumber) && sens->number > 0)
      s.sensitivity = float(sens->number);
  }
  return s;
}

}  // namespace

std::string_view Name(XboxControl c) { return kXboxNames[size_t(c)]; }
std::string_view Name(PadControl c) { return kPadNames[size_t(c)]; }

std::optional<XboxControl> ParseXboxControl(std::string_view name) {
  for (size_t i = 0; i < std::size(kXboxNames); ++i)
    if (kXboxNames[i] == name) return XboxControl(i);
  return std::nullopt;
}

std::optional<PadControl> ParsePadControl(std::string_view name) {
  for (size_t i = 0; i < std::size(kPadNames); ++i)
    if (kPadNames[i] == name) return PadControl(i);
  return std::nullopt;
}

InputBindings InputBindings::Parse(std::string_view text, std::vector<std::string>* problems) {
  InputBindings out;
  json::Value root;
  std::string error;
  if (!json::Parse(text, root, &error)) {
    Problem(problems, "not JSON: " + error);
    return out;
  }
  const json::Value* version = root.Find("version");
  if (!version || !version->is(json::Value::Type::kNumber) || version->number != 1) {
    Problem(problems, "not a version 1 bindings file");
    return out;
  }
  const json::Value* profiles = root.Find("profiles");
  if (!profiles || !profiles->is(json::Value::Type::kObject)) {
    Problem(problems, "no \"profiles\" object");
    return out;
  }
  for (const auto& [profile_name, body] : profiles->object) {
    if (!body.is(json::Value::Type::kObject)) {
      Problem(problems, profile_name + ": a profile is an object");
      continue;
    }
    BindingProfile profile;
    profile.name = profile_name;
    for (const auto& [key, value] : body.object) {
      const std::string where = profile_name + "." + key;
      if (key == "base") {
        if (value.is(json::Value::Type::kString)) profile.base = value.string;
        else Problem(problems, where + ": a profile's base is a profile name");
        continue;
      }
      Binding binding;
      binding.target = key;
      if (key.starts_with("xbox:")) {
        binding.xbox = ParseXboxControl(std::string_view(key).substr(5));
        if (!binding.xbox) {
          Problem(problems, where + ": unknown Xbox control");
          continue;
        }
      } else if (!key.starts_with("action:") || key.size() == 7) {
        Problem(problems, where + ": a target is xbox:<control> or action:<name>");
        continue;
      }
      if (value.is(json::Value::Type::kArray)) {
        for (const json::Value& source : value.array)
          if (auto s = ParseSource(source, problems, where)) binding.sources.push_back(std::move(*s));
      } else if (auto s = ParseSource(value, problems, where)) {
        binding.sources.push_back(std::move(*s));
      }
      profile.bindings.push_back(std::move(binding));
    }
    out.profiles_.push_back(std::move(profile));
  }
  return out;
}

const BindingProfile* InputBindings::Find(std::string_view profile) const {
  for (const BindingProfile& p : profiles_)
    if (p.name == profile) return &p;
  return nullptr;
}

InputMapper::InputMapper(const InputBindings& bindings, std::string_view profile) {
  // The chain from the profile to its last base (a loop or more than 8 levels stops it), applied base first.
  std::vector<const BindingProfile*> chain;
  for (const BindingProfile* p = bindings.Find(profile); p && chain.size() < 8; p = p->base.empty() ? nullptr : bindings.Find(p->base)) {
    if (std::find(chain.begin(), chain.end(), p) != chain.end()) break;
    chain.push_back(p);
  }
  std::array<const Binding*, size_t(XboxControl::kCount)> chosen{};
  for (auto p = chain.rbegin(); p != chain.rend(); ++p)
    for (const Binding& b : (*p)->bindings)
      if (b.xbox) chosen[size_t(*b.xbox)] = &b;
  for (size_t t = 0; t < chosen.size(); ++t) {
    if (!chosen[t]) continue;
    Target& target = targets_[t];
    target.named = true;
    target.first = uint16_t(sources_.size());
    for (const BindingSource& s : chosen[t]->sources)
      if (s.kind == BindingSource::Kind::kPad) sources_.push_back({s.pad, s.invert, s.value});
    target.count = uint16_t(sources_.size() - target.first);
    // A target bound to its own control alone is the identity.
    const bool own = target.count == 1 && sources_[target.first].pad == kOwnPlace[t] && !sources_[target.first].invert &&
                     sources_[target.first].value == 1.0f;
    if (!own) identity_ = false;
  }
  held_.assign(sources_.size(), 0);
}

GamepadState InputMapper::Map(const GamepadState& physical) {
  if (identity_) return physical;
  GamepadState out = physical;  // a target the profile does not name keeps its own control
  for (size_t t = 0; t < targets_.size(); ++t) {
    const Target& target = targets_[t];
    if (!target.named) continue;
    const XboxControl x = XboxControl(t);
    bool pressed = false;
    int largest = 0;   // a trigger target, 0-255
    int64_t sum = 0;   // an axis target
    for (uint16_t i = target.first; i < target.first + target.count; ++i) {
      const Source& s = sources_[i];
      // As a button, with hysteresis.
      bool on;
      if (IsTrigger(s.pad)) {
        const int v = s.pad == PadControl::kLeftTrigger ? physical.left_trigger : physical.right_trigger;
        on = v > (held_[i] ? kTriggerOff : kTriggerOn);
      } else if (IsAxis(s.pad) || IsDirection(s.pad)) {
        on = Travel(physical, s.pad) > (held_[i] ? kStickOff : kStickOn);
      } else {
        on = (physical.buttons & PadBit(s.pad)) != 0;
      }
      held_[i] = on;
      if (IsXboxAxis(x)) {
        if (IsAxis(s.pad)) {
          const int v = Axis(physical, s.pad);
          sum += s.invert ? -int64_t(v) : int64_t(v);
        } else if (IsTrigger(s.pad)) {
          const int v = s.pad == PadControl::kLeftTrigger ? physical.left_trigger : physical.right_trigger;
          sum += int64_t(std::lround(double(s.value) * v * 32767.0 / 255.0));
        } else if (IsDirection(s.pad)) {
          sum += int64_t(std::lround(double(s.value) * Travel(physical, s.pad)));
        } else if (on) {
          sum += int64_t(std::lround(double(s.value) * 32767.0));
        }
      } else if (IsXboxTrigger(x)) {
        int v;
        if (IsTrigger(s.pad)) v = s.pad == PadControl::kLeftTrigger ? physical.left_trigger : physical.right_trigger;
        else if (IsAxis(s.pad) || IsDirection(s.pad)) v = std::min(Travel(physical, s.pad), 32767) * 255 / 32767;
        else v = on ? 255 : 0;
        largest = std::max(largest, v);
      } else {
        pressed |= on;
      }
    }
    switch (x) {
      case XboxControl::kLt: out.left_trigger = uint8_t(largest); break;
      case XboxControl::kRt: out.right_trigger = uint8_t(largest); break;
      case XboxControl::kLeftX: out.thumb_lx = int16_t(std::clamp<int64_t>(sum, -32768, 32767)); break;
      case XboxControl::kLeftY: out.thumb_ly = int16_t(std::clamp<int64_t>(sum, -32768, 32767)); break;
      case XboxControl::kRightX: out.thumb_rx = int16_t(std::clamp<int64_t>(sum, -32768, 32767)); break;
      case XboxControl::kRightY: out.thumb_ry = int16_t(std::clamp<int64_t>(sum, -32768, 32767)); break;
      default: out.buttons = uint16_t(pressed ? out.buttons | kXboxBit[t] : out.buttons & ~kXboxBit[t]); break;
    }
  }
  return out;
}

std::vector<PadControl> InputMapper::Bound(XboxControl target) const {
  const Target& t = targets_[size_t(target)];
  if (!t.named) return {kOwnPlace[size_t(target)]};
  std::vector<PadControl> pads;
  for (uint16_t i = t.first; i < t.first + t.count; ++i) pads.push_back(sources_[i].pad);
  return pads;
}

}  // namespace any360::sys
