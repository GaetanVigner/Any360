#include "any360/host/players.h"

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "any360/sys/input_players.h"

namespace any360::host::rexglue {

namespace {

using sys::ControllerFamily;
using Clock = std::chrono::steady_clock;

struct Known {
  rex::input::DeviceId id;
  uint32_t local;          // the device's number for PlayerSlots and the glyph query
  bool keyboard;           // a synthetic device: the keyboard and mouse (or the runtime's stand-in)
  ControllerFamily family;
  std::string name;
  std::string guid;
};

// Everything below is guarded by g_players. The runtime calls the assignment from inside its GetState, which the host
// calls holding LockHostInput; ObservePlayer never holds g_players across a GetState.
std::mutex g_players;
bool g_installed = false;
std::function<void(const std::string&)> g_log;
std::vector<Known> g_known;
uint32_t g_next_local = 1;
sys::PlayerSlots g_slots;
sys::GlyphQuery g_glyphs;
struct UserSeen {
  sys::GamepadState merged{}, pad{};
  ControllerFamily shown = ControllerFamily::kCount;
};
std::array<UserSeen, sys::PlayerSlots::kUsers> g_seen{};
// While set, the user's devices are its pad alone (ObservePlayer reading the pad apart from the keyboard).
uint32_t g_solo_user = sys::PlayerSlots::kNone;

void Log(const std::string& line) {
  if (g_log) g_log(line);
}

const Known* FindLocal(uint32_t local) {
  for (const Known& k : g_known)
    if (k.local == local) return &k;
  return nullptr;
}

// A pad's identity across connections: its GUID, and which of the pads sharing it (in connection order).
uint64_t Identity(const std::string& guid, size_t same_before) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : guid) h = (h ^ c) * 1099511628211ull;
  return h ^ (uint64_t(same_before) << 56);
}

std::string PlayerName(uint32_t user) { return "player " + std::to_string(user + 1); }

class PlayerAssignment final : public rex::input::DeviceAssignment {
 public:
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override {
    std::lock_guard lock(g_players);
    // Gone.
    for (size_t i = 0; i < g_known.size();) {
      const Known k = g_known[i];
      const bool present = std::any_of(devices.begin(), devices.end(), [&](const auto& d) { return d.id == k.id; });
      if (present) {
        ++i;
        continue;
      }
      const uint32_t user = g_slots.UserOf(k.local);
      if (user != sys::PlayerSlots::kNone) g_glyphs.user(user).Disconnected(k.local);
      const uint32_t placed = g_slots.Disconnected(k.local);
      g_known.erase(g_known.begin() + i);
      Log(k.name + " left" + (user != sys::PlayerSlots::kNone ? " (" + PlayerName(user) + ")" : std::string()));
      if (placed != sys::PlayerSlots::kNone) {
        if (const Known* w = FindLocal(placed)) {
          const uint32_t u = g_slots.UserOf(placed);
          g_glyphs.user(u).Connected(placed, w->family);
          Log(w->name + " is now " + PlayerName(u));
        }
      }
    }
    // New, in the runtime's order.
    for (const rex::input::DeviceInfo& d : devices) {
      if (std::any_of(g_known.begin(), g_known.end(), [&](const Known& k) { return k.id == d.id; })) continue;
      Known k{d.id, g_next_local++, d.synthetic, ControllerFamily::kKeyboardMouse, d.name, d.guid};
      uint32_t user;
      std::string how;
      if (k.keyboard) {
        user = g_slots.ConnectKeyboard(k.local);
        how = "keyboard and mouse";
      } else {
        const sys::PadDescription desc = sys::DescribePad(d.guid, d.name);
        k.family = sys::FamilyOf(desc);
        size_t same = 0;
        for (const Known& other : g_known)
          if (!other.keyboard && other.guid == d.guid) ++same;
        user = g_slots.ConnectPad(k.local, Identity(d.guid, same));
        char ids[32];
        std::snprintf(ids, sizeof ids, "%04X:%04X", desc.vendor, desc.product);
        how = std::string(sys::Name(k.family)) + ", " + ids + ", GUID " + d.guid;
      }
      g_known.push_back(k);
      if (user != sys::PlayerSlots::kNone) g_glyphs.user(user).Connected(k.local, k.family);
      Log(k.name + " (" + how + ") " + (user != sys::PlayerSlots::kNone ? "is " + PlayerName(user) : "waits for a free player"));
    }
  }

  void DevicesForUser(uint32_t user, std::vector<rex::input::DeviceId>& out) const override {
    std::lock_guard lock(g_players);
    out.clear();
    if (user >= sys::PlayerSlots::kUsers) return;
    if (const Known* pad = FindLocal(g_slots.PadIn(user))) out.push_back(pad->id);
    if (g_solo_user == user) return;
    if (user == g_slots.keyboard_user())
      for (const Known& k : g_known)
        if (k.keyboard) out.push_back(k.id);
  }
};

sys::GamepadState FromX(const rex::input::X_INPUT_STATE& s) {
  sys::GamepadState pad;
  pad.buttons = s.gamepad.buttons;
  pad.left_trigger = s.gamepad.left_trigger;
  pad.right_trigger = s.gamepad.right_trigger;
  pad.thumb_lx = s.gamepad.thumb_lx;
  pad.thumb_ly = s.gamepad.thumb_ly;
  pad.thumb_rx = s.gamepad.thumb_rx;
  pad.thumb_ry = s.gamepad.thumb_ry;
  return pad;
}

}  // namespace

void InstallPlayers(rex::input::InputSystem* input, std::function<void(const std::string&)> log) {
  {
    std::lock_guard lock(g_players);
    if (g_installed || !input) return;
    g_installed = true;
    g_log = std::move(log);
  }
  input->SetDeviceAssignment(std::make_unique<PlayerAssignment>());
}

bool PlayersInstalled() {
  std::lock_guard lock(g_players);
  return g_installed;
}

void ObservePlayer(rex::input::InputSystem* input, uint32_t user, const sys::GamepadState& merged) {
  if (user >= sys::PlayerSlots::kUsers) return;
  uint32_t pad_local, keyboard_local = sys::PlayerSlots::kNone;
  {
    std::lock_guard lock(g_players);
    if (!g_installed) return;
    pad_local = g_slots.PadIn(user);
    if (user == g_slots.keyboard_user())
      for (const Known& k : g_known)
        if (k.keyboard) keyboard_local = k.local;
  }
  const bool both = pad_local != sys::PlayerSlots::kNone && keyboard_local != sys::PlayerSlots::kNone;
  // The pad alone, when the keyboard shares the user: its own changes tell which device the player used.
  sys::GamepadState pad = merged;
  if (both && input) {
    {
      std::lock_guard lock(g_players);
      g_solo_user = user;
    }
    rex::input::X_INPUT_STATE alone{};
    const bool read = input->GetState(user, &alone) == 0;
    {
      std::lock_guard lock(g_players);
      g_solo_user = sys::PlayerSlots::kNone;
    }
    pad = read ? FromX(alone) : sys::GamepadState{};
  }
  const auto now = Clock::now();
  std::lock_guard lock(g_players);
  UserSeen& seen = g_seen[user];
  const bool pad_real = pad_local != sys::PlayerSlots::kNone && sys::IsRealInput(seen.pad, pad);
  const bool merged_real = sys::IsRealInput(seen.merged, merged);
  if (pad_real) {
    g_glyphs.user(user).Input(pad_local, now);
  } else if (merged_real) {
    const uint32_t device = keyboard_local != sys::PlayerSlots::kNone ? keyboard_local : pad_local;
    if (device != sys::PlayerSlots::kNone) g_glyphs.user(user).Input(device, now);
  }
  seen.pad = pad;
  seen.merged = merged;
  const ControllerFamily shown = g_glyphs.user(user).Shown(now);
  if (shown != seen.shown) {
    if (seen.shown != ControllerFamily::kCount) Log(PlayerName(user) + "'s prompts now " + std::string(sys::Name(shown)));
    seen.shown = shown;
  }
}

ControllerFamily PlayerFamily(uint32_t user) {
  std::lock_guard lock(g_players);
  if (!g_installed) return ControllerFamily::kCount;
  const Known* pad = FindLocal(g_slots.PadIn(user));
  return pad ? pad->family : ControllerFamily::kCount;
}

sys::Glyph PlayerGlyph(uint32_t user, sys::XboxControl control) {
  std::lock_guard lock(g_players);
  return g_glyphs.For(user, control, Clock::now());
}

uint32_t PlayerGlyphGeneration() {
  std::lock_guard lock(g_players);
  return g_glyphs.Generation(Clock::now());
}

void SetKeyboardPlayer(uint32_t user) {
  std::lock_guard lock(g_players);
  g_slots.set_keyboard_user(user);
}

void SetPlayerBindings(const sys::InputBindings& bindings) {
  std::lock_guard lock(g_players);
  g_glyphs.SetBindings(bindings);
}

}  // namespace any360::host::rexglue
