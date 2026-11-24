// Which player each device is (docs/GLYPHS.md, section 1, "Several players"): the four user slots a title reads
// (XInputGetState's user index), filled by the host's pads, stable through unplugging so that split-screen players
// never swap places. Plain data, no host types: the host gives each pad's connection (`device`, for this session) and
// its identity (`identity`, stable across connections: SDL's GUID and serial number, or its path), any360::sys answers
// the user.
//
// - A pad takes the lowest slot neither occupied nor reserved; none left, the oldest reservation.
// - A pad unplugged keeps its slot reserved for its identity: the others stay where they are, and the same pad
//   plugged back takes its slot again (when another pad took it meanwhile, it is a new pad).
// - Pads beyond the four wait, in order, and take a slot as soon as one frees.
// - The keyboard and mouse join one player (kKeyboardUser by default, the first) without taking a slot.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace any360::sys {

class PlayerSlots {
 public:
  static constexpr uint32_t kUsers = 4;
  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  // A pad connected: its user, or kNone while it waits for a slot.
  uint32_t ConnectPad(uint32_t device, uint64_t identity);
  // The keyboard (or mouse) connected: the user it joins.
  uint32_t ConnectKeyboard(uint32_t device);
  // A device disconnected. Returns the device that took a freed slot (a waiting pad), or kNone.
  uint32_t Disconnected(uint32_t device);

  // The user a device feeds, kNone when it waits or is unknown.
  uint32_t UserOf(uint32_t device) const;
  // Whether a pad sits in the slot (XInput's "connected" for that user). The keyboard's user counts as connected.
  bool Connected(uint32_t user) const;
  // The pad in a slot (kNone when empty).
  uint32_t PadIn(uint32_t user) const { return user < kUsers ? slots_[user].device : kNone; }
  void set_keyboard_user(uint32_t user) { keyboard_user_ = user < kUsers ? user : 0; }
  uint32_t keyboard_user() const { return keyboard_user_; }

 private:
  struct Slot {
    uint32_t device = kNone;   // the pad in it
    uint64_t identity = 0;     // its pad's identity, kept while reserved
    bool reserved = false;     // left by an unplugged pad
    uint64_t released = 0;     // when (an order, not a time)
  };
  struct Waiting {
    uint32_t device;
    uint64_t identity;
  };
  uint32_t Place(uint32_t device, uint64_t identity);

  std::array<Slot, kUsers> slots_{};
  std::vector<Waiting> waiting_;
  std::vector<uint32_t> keyboards_;
  uint32_t keyboard_user_ = 0;
  uint64_t order_ = 0;
};

}  // namespace any360::sys
