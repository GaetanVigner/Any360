// Several players through the reference runtime's own InputSystem (players.h): a driver of fake devices, Any360's
// assignment installed on it. Split screen, a pad unplugged and back, the keyboard sharing player 1, each player's
// family and prompts.
// Run: host_players_tests
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "any360/host/players.h"

#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>

using namespace any360::host::rexglue;

namespace {

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

class FakeDriver final : public rex::input::InputDriver {
 public:
  FakeDriver() : InputDriver(nullptr, 0) {}
  rex::X_STATUS Setup() override { return 0; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override { out = devices; }
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    for (size_t i = 0; i < devices.size(); ++i)
      if (devices[i].id == id) {
        *out = {};
        out->gamepad.buttons = buttons[i];
        return 0;
      }
    return 0x48F;  // not connected
  }
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override { return 0; }
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override { return 0; }
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override { return 0x10D2; }
  void Add(uint64_t id, const char* name, const char* guid, bool synthetic) {
    rex::input::DeviceInfo d;
    d.id = rex::input::DeviceId(id);
    d.name = name;
    d.guid = guid;
    d.synthetic = synthetic;
    devices.push_back(d);
    buttons.push_back(0);
  }
  void Remove(uint64_t id) {
    for (size_t i = 0; i < devices.size(); ++i)
      if (devices[i].id == rex::input::DeviceId(id)) {
        devices.erase(devices.begin() + i);
        buttons.erase(buttons.begin() + i);
        return;
      }
  }
  void Press(uint64_t id, uint16_t b) {
    for (size_t i = 0; i < devices.size(); ++i)
      if (devices[i].id == rex::input::DeviceId(id)) buttons[i] = b;
  }
  std::vector<rex::input::DeviceInfo> devices;
  std::vector<uint16_t> buttons;
};

}  // namespace

int main() {
  using any360::sys::ControllerFamily;
  using any360::sys::XboxControl;
  // SDL GUIDs: USB, Valve's vendor 0x28DE, product 0x1205 (Steam Deck) and 0x1142 (Steam Controller).
  const char* kDeck = "03000000de2800000512000000000000";
  const char* kSteamController = "03000000de2800004211000000000000";
  rex::input::InputSystem input(nullptr);
  auto owned = std::make_unique<FakeDriver>();
  FakeDriver* fake = owned.get();
  input.AddDriver(std::move(owned));
  std::vector<std::string> log;
  InstallPlayers(&input, [&](const std::string& line) { log.push_back(line); });
  CHECK(PlayersInstalled());
  // A poll as the host makes it: the merged state, then which device gave it.
  auto state = [&](uint32_t user, uint16_t& buttons) {
    rex::input::X_INPUT_STATE s{};
    const bool ok = input.GetState(user, &s) == 0;
    buttons = s.gamepad.buttons;
    any360::sys::GamepadState pad;
    pad.buttons = ok ? uint16_t(s.gamepad.buttons) : uint16_t(0);
    ObservePlayer(&input, user, pad);
    return ok;
  };
  uint16_t b = 0;
  // Split screen: the Deck is player 1, the Steam Controller player 2, each with its own state and family.
  fake->Add(1, "Steam Deck", kDeck, false);
  fake->Add(2, "Steam Controller", kSteamController, false);
  fake->Press(1, 0x1000);
  CHECK(state(0, b) && b == 0x1000 && state(1, b) && b == 0 && !state(2, b));
  CHECK(PlayerFamily(0) == ControllerFamily::kSteamDeck && PlayerFamily(1) == ControllerFamily::kSteamController);
  CHECK(PlayerFamily(2) == ControllerFamily::kCount);
  CHECK(PlayerGlyph(0, XboxControl::kA).set == "steam_deck" && PlayerGlyph(1, XboxControl::kA).set == "steam_controller");
  // Player 1's pad unplugged: player 2 stays player 2; plugged back (a new connection), it is player 1 again.
  fake->Remove(1);
  CHECK(!state(0, b) && state(1, b));
  fake->Press(2, 0x2000);
  CHECK(state(1, b) && b == 0x2000);
  fake->Add(3, "Steam Deck", kDeck, false);
  CHECK(state(0, b) && b == 0 && PlayerFamily(0) == ControllerFamily::kSteamDeck && state(1, b) && b == 0x2000);
  // The keyboard joins player 1: its presses reach player 1's merged state, player 1's prompts follow it after the
  // debounce, player 2's stay on the Steam Controller; player 1's bindings stay the pad's.
  fake->Add(4, "Keyboard and mouse", "", true);
  fake->Press(4, 0x4000);
  CHECK(state(0, b) && b == 0x4000 && state(1, b) && b == 0x2000);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(state(0, b));
  CHECK(PlayerGlyph(0, XboxControl::kA).family == ControllerFamily::kKeyboardMouse);
  CHECK(PlayerGlyph(1, XboxControl::kA).family == ControllerFamily::kSteamController);
  CHECK(PlayerFamily(0) == ControllerFamily::kSteamDeck);
  // Back on the pad: its own change is told from the keyboard's, the prompts follow it.
  fake->Press(4, 0);
  fake->Press(3, 0x1000);
  CHECK(state(0, b) && b == 0x1000);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(state(0, b) && PlayerGlyph(0, XboxControl::kA).family == ControllerFamily::kSteamDeck);
  // The bindings the glyphs follow: player 1's A on the Deck's east button.
  SetPlayerBindings(any360::sys::InputBindings::Parse(
      R"({"version":1,"profiles":{"steam_deck":{"xbox:a":["east"]}}})"));
  CHECK(PlayerGlyph(0, XboxControl::kA).control == "east" && PlayerGlyph(1, XboxControl::kA).control == "south");
  CHECK(log.size() >= 6);
  for (const std::string& line : log) std::printf("host_players_tests: %s\n", line.c_str());
  std::printf("host_players_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
