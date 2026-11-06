// A controller's input as a queue, behind the console's own calls (XamInputGetState, XamInputGetKeystroke): nothing
// for a title to call or hook. A title made for a console polls at most once per vertical blank; run faster than that
// (unlocked frame rate) its logic may step slower than it polls, and a change seen by one poll only can fall between
// two logic steps. So:
//
// - The controller is sampled on its own thread (Sample), far faster than any title polls. Every change of a button,
//   and of a trigger past its threshold (with hysteresis), is queued. The sticks queue nothing and are always
//   presented live: they cannot fill the queue faster than it is read, a camera stick never lags behind, a released
//   stick is released at the next poll.
// - A poll (Present) takes the queue one step at a time: a step takes the queued changes in order as long as no input
//   changes twice, so inputs pressed together arrive together while a second change of the same input (a double tap)
//   waits for the next step. Steps are at least one console frame apart (the step period): every change stays
//   visible for at least one console frame, as a pad polled on the console would show it, so a title stepping its
//   logic at up to the console's rate sees each one. A trigger shows the step's value only while a change of it waits.
// - Keystrokes are made from every sampled change, the sticks' eight directions included (no step: they are a queue
//   already).
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>

namespace any360::sys {

struct GamepadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
  bool operator==(const GamepadState&) const = default;
};

class InputQueue {
 public:
  using Clock = std::chrono::steady_clock;
  // One console frame (59.94 Hz).
  static constexpr Clock::duration kConsoleFrame = std::chrono::microseconds(16683);
  static constexpr size_t kCapacity = 256;

  // The controller as sampled at `at` (the sampling thread, or a poll when nothing samples).
  void Sample(const GamepadState& state, Clock::time_point at);
  // What a poll at `at` presents, stepping the queue if `step` has passed since the last step (0: every poll steps).
  GamepadState Present(Clock::time_point at, Clock::duration step = kConsoleFrame);
  // The next keystroke (virtual key, XINPUT_KEYSTROKE flags); false when none. Repeats are made here, from `at`.
  bool NextKeystroke(Clock::time_point at, uint16_t& key, uint16_t& flags);

  size_t pending() const { return queue_.size(); }
  void Clear() { *this = InputQueue{}; }

  // The digital inputs of a state (bit layout below), with hysteresis against the previous ones.
  static uint32_t Digital(const GamepadState& state, uint32_t previous);
  // Bits: buttons 0-15, left trigger 16, right trigger 17, left stick up/down/right/left 18-21, right stick 22-25.
  static constexpr uint32_t kLeftTrigger = 1u << 16, kRightTrigger = 1u << 17, kLeftStick = 18, kRightStick = 22;
  // The inputs that queue: the buttons and the triggers.
  static constexpr uint32_t kQueued = 0x3FFFF;

 private:
  struct Change {
    GamepadState state;
    uint32_t digital;
  };
  void Keystrokes(uint32_t from, uint32_t to);

  GamepadState latest_{};
  uint32_t sampled_digital_ = 0;
  std::deque<Change> queue_;
  GamepadState presented_{};
  uint32_t presented_digital_ = 0;
  bool stepped_ = false;
  Clock::time_point last_step_{};
  // Keystrokes: (virtual key, flags), the key repeating and when.
  std::deque<std::array<uint16_t, 2>> keystrokes_;
  uint16_t repeat_key_ = 0;
  Clock::time_point repeat_at_{};
  Clock::time_point last_sample_{};
};

}  // namespace any360::sys
