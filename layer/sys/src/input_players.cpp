#include "any360/sys/input_players.h"

#include <algorithm>

namespace any360::sys {

uint32_t PlayerSlots::Place(uint32_t device, uint64_t identity) {
  // Its own reserved slot first.
  for (uint32_t u = 0; u < kUsers; ++u) {
    Slot& s = slots_[u];
    if (s.device == kNone && s.reserved && s.identity == identity) {
      s = {device, identity, false, 0};
      return u;
    }
  }
  // The lowest free one.
  for (uint32_t u = 0; u < kUsers; ++u) {
    Slot& s = slots_[u];
    if (s.device == kNone && !s.reserved) {
      s = {device, identity, false, 0};
      return u;
    }
  }
  // The oldest reservation.
  uint32_t oldest = kNone;
  for (uint32_t u = 0; u < kUsers; ++u)
    if (slots_[u].device == kNone && (oldest == kNone || slots_[u].released < slots_[oldest].released)) oldest = u;
  if (oldest != kNone) slots_[oldest] = {device, identity, false, 0};
  return oldest;
}

uint32_t PlayerSlots::ConnectPad(uint32_t device, uint64_t identity) {
  if (const uint32_t u = UserOf(device); u != kNone) return u;  // already placed
  for (const Waiting& w : waiting_)
    if (w.device == device) return kNone;
  const uint32_t u = Place(device, identity);
  if (u == kNone) waiting_.push_back({device, identity});
  return u;
}

uint32_t PlayerSlots::ConnectKeyboard(uint32_t device) {
  if (std::find(keyboards_.begin(), keyboards_.end(), device) == keyboards_.end()) keyboards_.push_back(device);
  return keyboard_user_;
}

uint32_t PlayerSlots::Disconnected(uint32_t device) {
  if (auto k = std::find(keyboards_.begin(), keyboards_.end(), device); k != keyboards_.end()) {
    keyboards_.erase(k);
    return kNone;
  }
  for (auto w = waiting_.begin(); w != waiting_.end(); ++w)
    if (w->device == device) {
      waiting_.erase(w);
      return kNone;
    }
  for (Slot& s : slots_) {
    if (s.device != device) continue;
    s.device = kNone;
    s.reserved = true;
    s.released = ++order_;
    // The first waiting pad takes a slot (its own reservation if it has one, else this one).
    if (!waiting_.empty()) {
      const Waiting w = waiting_.front();
      waiting_.erase(waiting_.begin());
      Place(w.device, w.identity);
      return w.device;
    }
    return kNone;
  }
  return kNone;
}

uint32_t PlayerSlots::UserOf(uint32_t device) const {
  for (uint32_t u = 0; u < kUsers; ++u)
    if (slots_[u].device == device) return u;
  if (std::find(keyboards_.begin(), keyboards_.end(), device) != keyboards_.end()) return keyboard_user_;
  return kNone;
}

bool PlayerSlots::Connected(uint32_t user) const {
  if (user >= kUsers) return false;
  return slots_[user].device != kNone || (user == keyboard_user_ && !keyboards_.empty());
}

}  // namespace any360::sys
