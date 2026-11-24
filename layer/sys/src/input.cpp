#include "any360/sys/input.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstdlib>

namespace any360::sys {

InputScript InputScript::Parse(const std::string& text, std::string* error) {
  static constexpr struct {
    const char* name;
    uint16_t bit;
  } kButtons[] = {{"up", 0x0001},     {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"start", 0x0010},
                  {"back", 0x0020},   {"ls", 0x0040},   {"rs", 0x0080},   {"lb", 0x0100},    {"rb", 0x0200},
                  {"a", 0x1000},      {"b", 0x2000},    {"x", 0x4000},    {"y", 0x8000}};
  InputScript script;
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = std::min(text.find(',', at), text.size());
    const std::string entry = text.substr(at, end - at);
    at = end + 1;
    const size_t colon = entry.find(':');
    bool ok = colon != std::string::npos && colon > 0;
    Press press{};
    press.hold = kPressDefault;
    if (ok) {
      char* rest = nullptr;
      const double seconds = std::strtod(entry.c_str(), &rest);
      ok = rest == entry.c_str() + colon && seconds >= 0;
      press.at = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }
    std::string buttons = ok ? entry.substr(colon + 1) : std::string();
    if (const size_t slash = buttons.find('/'); ok && slash != std::string::npos) {
      const long ms = std::strtol(buttons.c_str() + slash + 1, nullptr, 10);
      ok = ms > 0;
      press.hold = std::chrono::milliseconds(ms);
      buttons.resize(slash);
    }
    for (size_t b = 0; ok && b < buttons.size();) {
      const size_t plus = std::min(buttons.find('+', b), buttons.size());
      const std::string name = buttons.substr(b, plus - b);
      b = plus + 1;
      bool known = false;
      for (const auto& k : kButtons)
        if (name == k.name) press.buttons |= k.bit, known = true;
      ok = known;
    }
    if (ok && press.buttons) {
      script.presses_.push_back(press);
    } else if (error && !entry.empty()) {
      *error += (error->empty() ? "" : ", ") + entry;
    }
  }
  return script;
}

namespace {

// VK_PAD_* for the button bits, in bit order (0 where the bit has no key).
constexpr uint16_t kButtonKeys[16] = {0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815, 0x5816, 0x5817,
                                      0x5805, 0x5804, 0,      0,      0x5800, 0x5801, 0x5802, 0x5803};
constexpr uint16_t kKeyDown = 1, kKeyUp = 2, kKeyRepeat = 4;
// XINPUT_GAMEPAD_TRIGGER_THRESHOLD, and half a stick's travel; an input on stays on until a little below.
constexpr int kTriggerOn = 30, kTriggerOff = 20, kStickOn = 0x4000, kStickOff = 0x3000;

uint32_t Stick(int16_t x, int16_t y, uint32_t previous) {
  auto past = [](int value, bool was) { return value > (was ? kStickOff : kStickOn); };
  return uint32_t(past(y, previous & 1)) | uint32_t(past(-y, previous & 2)) << 1 | uint32_t(past(x, previous & 4)) << 2 |
         uint32_t(past(-x, previous & 8)) << 3;
}

// A stick's direction bits (up, down, right, left) as VK_PAD_xTHUMB_* (eight directions), or 0.
uint16_t StickKey(uint16_t base, uint32_t bits) {
  const bool up = bits & 1, down = bits & 2, right = bits & 4, left = bits & 8;
  if (up && left) return uint16_t(base + 4);
  if (up && right) return uint16_t(base + 5);
  if (down && right) return uint16_t(base + 6);
  if (down && left) return uint16_t(base + 7);
  if (up) return base;
  if (down) return uint16_t(base + 1);
  if (right) return uint16_t(base + 2);
  if (left) return uint16_t(base + 3);
  return 0;
}

// The keys down for digital inputs, by slot (a key's low six bits: buttons 0x00-0x17, sticks 0x20-0x27 and 0x30-0x37).
std::array<bool, 64> KeysDown(uint32_t digital) {
  std::array<bool, 64> down{};
  for (uint32_t bit = 0; bit < 16; ++bit) {
    if (kButtonKeys[bit] && (digital >> bit & 1)) down[kButtonKeys[bit] & 0x3F] = true;
  }
  if (digital & InputQueue::kLeftTrigger) down[0x06] = true;
  if (digital & InputQueue::kRightTrigger) down[0x07] = true;
  if (const uint16_t k = StickKey(0x5820, digital >> InputQueue::kLeftStick & 0xF)) down[k & 0x3F] = true;
  if (const uint16_t k = StickKey(0x5830, digital >> InputQueue::kRightStick & 0xF)) down[k & 0x3F] = true;
  return down;
}

}  // namespace

uint32_t InputQueue::Digital(const GamepadState& s, uint32_t previous) {
  uint32_t d = s.buttons;
  if (s.left_trigger > ((previous & kLeftTrigger) ? kTriggerOff : kTriggerOn)) d |= kLeftTrigger;
  if (s.right_trigger > ((previous & kRightTrigger) ? kTriggerOff : kTriggerOn)) d |= kRightTrigger;
  d |= Stick(s.thumb_lx, s.thumb_ly, previous >> kLeftStick & 0xF) << kLeftStick;
  d |= Stick(s.thumb_rx, s.thumb_ry, previous >> kRightStick & 0xF) << kRightStick;
  return d;
}

void InputQueue::Sample(const GamepadState& state, Clock::time_point at) {
  const uint32_t digital = Digital(state, sampled_digital_);
  latest_ = state;
  last_sample_ = at;
  if (digital == sampled_digital_) return;
  Keystrokes(sampled_digital_, digital);
  const bool queued = (digital ^ sampled_digital_) & kQueued;
  sampled_digital_ = digital;
  if (!queued) return;  // a stick's direction: the sticks are live
  if (queue_.size() == kCapacity) {
    // Not read for this long: the oldest change is presented without a step of its own.
    presented_ = queue_.front().state;
    presented_digital_ = queue_.front().digital;
    queue_.pop_front();
  }
  queue_.push_back({state, digital & kQueued});
}

// A key down or up per changed key; the key pressed last repeats (400 ms after its press, then every 100 ms) while held.
void InputQueue::Keystrokes(uint32_t from, uint32_t to) {
  const auto before = KeysDown(from), now = KeysDown(to);
  for (uint16_t i = 0; i < 64; ++i) {
    if (now[i] == before[i]) continue;
    const uint16_t key = uint16_t(0x5800 | i);
    if (keystrokes_.size() < kCapacity) keystrokes_.push_back({key, now[i] ? kKeyDown : kKeyUp});
    if (now[i]) {
      repeat_key_ = key;
      repeat_at_ = last_sample_ + std::chrono::milliseconds(400);
    } else if (repeat_key_ == key) {
      repeat_key_ = 0;
    }
  }
}

GamepadState InputQueue::Present(Clock::time_point at, Clock::duration step) {
  if (!queue_.empty() && (!stepped_ || at - last_step_ >= step)) {
    uint32_t changed = 0;
    while (!queue_.empty()) {
      const Change& next = queue_.front();
      const uint32_t diff = next.digital ^ presented_digital_;
      if (diff & changed) break;
      changed |= diff;
      presented_ = next.state;
      presented_digital_ = next.digital;
      queue_.pop_front();
    }
    stepped_ = true;
    last_step_ = at;
  }
  // The sticks are live; the buttons are the step's; a trigger is the step's only while a change of it waits.
  GamepadState out = latest_;
  if (!queue_.empty()) {
    out.buttons = presented_.buttons;
    const uint32_t live = sampled_digital_ & kQueued;
    if ((live ^ presented_digital_) & kLeftTrigger) out.left_trigger = presented_.left_trigger;
    if ((live ^ presented_digital_) & kRightTrigger) out.right_trigger = presented_.right_trigger;
  }
  return out;
}

bool InputQueue::NextKeystroke(Clock::time_point at, uint16_t& key, uint16_t& flags) {
  if (keystrokes_.empty() && repeat_key_ && at >= repeat_at_) {
    keystrokes_.push_back({repeat_key_, uint16_t(kKeyDown | kKeyRepeat)});
    repeat_at_ = at + std::chrono::milliseconds(100);
  }
  if (keystrokes_.empty()) return false;
  key = keystrokes_.front()[0];
  flags = keystrokes_.front()[1];
  keystrokes_.pop_front();
  return true;
}

void InputRecording::Add(Clock::duration at, const GamepadState& state) {
  if (!events_.empty()) {
    if (state == events_.back().state) return;
    at = std::max(at, events_.back().at);
  } else if (state == GamepadState{}) {
    return;  // neutral before the first event anyway
  }
  events_.push_back({at, state});
}

GamepadState InputRecording::StateAt(Clock::duration since) const {
  // The last event at or before `since`.
  const auto after = std::upper_bound(events_.begin(), events_.end(), since,
                                      [](Clock::duration t, const Event& e) { return t < e.at; });
  return after == events_.begin() ? GamepadState{} : std::prev(after)->state;
}

std::string InputRecording::Text(size_t from) const {
  std::string text;
  if (from == 0) text = std::string(kHeader) + "\n";
  char line[96];
  for (size_t i = from; i < events_.size(); ++i) {
    const Event& e = events_[i];
    const GamepadState& s = e.state;
    std::snprintf(line, sizeof line, "%lld %04X %u %u %d %d %d %d\n",
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(e.at).count()),
                  unsigned(s.buttons), unsigned(s.left_trigger), unsigned(s.right_trigger), int(s.thumb_lx),
                  int(s.thumb_ly), int(s.thumb_rx), int(s.thumb_ry));
    text += line;
  }
  return text;
}

bool InputRecording::Parse(const std::string& text, InputRecording& out, std::string* error) {
  out = InputRecording{};
  size_t at = 0, number = 0;
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    ++number;
    if (number == 1) {
      if (line != kHeader) return fail("not an Any360 input recording (first line: " + line + ")");
      continue;
    }
    if (line.empty()) continue;
    long long us = 0;
    unsigned buttons = 0, lt = 0, rt = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    if (std::sscanf(line.c_str(), "%lld %x %u %u %d %d %d %d", &us, &buttons, &lt, &rt, &lx, &ly, &rx, &ry) != 8 ||
        us < 0 || buttons > 0xFFFF || lt > 255 || rt > 255 || lx < -32768 || lx > 32767 || ly < -32768 || ly > 32767 ||
        rx < -32768 || rx > 32767 || ry < -32768 || ry > 32767) {
      return fail("line " + std::to_string(number) + " not understood: " + line);
    }
    const GamepadState state{uint16_t(buttons), uint8_t(lt), uint8_t(rt), int16_t(lx), int16_t(ly), int16_t(rx), int16_t(ry)};
    out.events_.push_back({std::chrono::microseconds(us), state});  // as recorded (a neutral first one included)
  }
  if (number == 0) return fail("empty");
  return true;
}

}  // namespace any360::sys
