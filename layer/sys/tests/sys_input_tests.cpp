// The input bindings (docs/GLYPHS.md, section 2): the JSON reader, the bindings file and layer 1's mapper, against
// the documented behaviour; then a sample's cost with and without bindings (time and host allocations a call).
//   sys_input_tests [--bench]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/input_bindings.h"
#include "any360/sys/input_glyphs.h"
#include "any360/sys/input_players.h"
#include "any360/sys/json.h"

using namespace any360::sys;

namespace {

int g_failures = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::printf("sys_input_tests: FAILED line %d: %s\n", __LINE__, #c); \
      ++g_failures;                                                      \
    }                                                                    \
  } while (0)

bool ParseOk(const char* text, json::Value& v) { return json::Parse(text, v); }
bool ParseFails(const char* text) {
  json::Value v;
  std::string error;
  return !json::Parse(text, v, &error) && !error.empty();
}

void JsonTests() {
  json::Value v;
  CHECK(ParseOk(R"( {"a": [1, -2.5e2, true, false, null, "x"], "b": {}, "c": []} )", v));
  CHECK(v.is(json::Value::Type::kObject) && v.object.size() == 3);
  const json::Value* a = v.Find("a");
  CHECK(a && a->array.size() == 6 && a->array[0].number == 1 && a->array[1].number == -250 && a->array[2].boolean &&
        a->array[3].is(json::Value::Type::kBool) && !a->array[3].boolean && a->array[4].is(json::Value::Type::kNull) &&
        a->array[5].string == "x");
  CHECK(v.Find("b")->is(json::Value::Type::kObject) && v.Find("c")->array.empty() && !v.Find("d"));
  // Escapes: the short ones, \u in the BMP, a surrogate pair (U+1F600), all as UTF-8.
  CHECK(ParseOk(R"("q\"b\\s\/n\nt\tu\u00e9\u20ac\ud83d\ude00")", v));
  CHECK(v.string == "q\"b\\s/n\nt\tu\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
  // Order kept, the first of two equal names found.
  CHECK(ParseOk(R"({"z":1,"a":2,"z":3})", v) && v.object[0].first == "z" && v.object[1].first == "a" && v.Find("z")->number == 1);
  // Malformed: each one refused with a place.
  for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "\"open", "\"tab\there\"", "01", "1.", "1e", "-",
                          "tru", "nul", "{} x", "\"\\ud800\"", "\"\\udc00\"", "\"\\x\"", "\"\\u12\"", "[1 2]", "1e999"}) {
    if (!ParseFails(bad)) std::printf("sys_input_tests: accepted %s\n", bad);
    CHECK(ParseFails(bad));
  }
  std::string error;
  CHECK(!json::Parse("{\n  \"a\": x}", v, &error) && error.starts_with("line 2, column 8"));
  // Nesting bounded: 64 levels read, 65 refused.
  CHECK(json::Parse(std::string(64, '[') + std::string(64, ']'), v));
  CHECK(ParseFails((std::string(65, '[') + std::string(65, ']')).c_str()));
}

GamepadState Pad(uint16_t buttons = 0) {
  GamepadState s;
  s.buttons = buttons;
  return s;
}

void BindingsTests() {
  constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kLb = 0x0100, kUp = 0x0001, kDown = 0x0002, kStart = 0x0010;
  // Every control name both ways.
  for (int i = 0; i < int(XboxControl::kCount); ++i) CHECK(ParseXboxControl(Name(XboxControl(i))) == XboxControl(i));
  for (int i = 0; i < int(PadControl::kCount); ++i) CHECK(ParsePadControl(Name(PadControl(i))) == PadControl(i));
  CHECK(!ParseXboxControl("c") && !ParsePadControl("south_east"));

  // Not a bindings file: no profiles, a reason.
  std::vector<std::string> problems;
  CHECK(InputBindings::Parse("[]", &problems).profiles().empty() && problems.size() == 1);
  problems.clear();
  CHECK(InputBindings::Parse(R"({"version":2,"profiles":{}})", &problems).profiles().empty() && problems.size() == 1);
  problems.clear();
  CHECK(InputBindings::Parse("{oops", &problems).profiles().empty() && problems[0].starts_with("not JSON"));

  // A file with every kind of entry, and bad ones skipped with a reason each.
  problems.clear();
  const InputBindings b = InputBindings::Parse(R"({
    "version": 1,
    "profiles": {
      "gamepad": {
        "xbox:a": ["east"], "xbox:b": "south",
        "xbox:lb": ["left_trigger"], "xbox:lt": ["left_shoulder"],
        "xbox:right_y": [{"control": "right_y", "invert": true}],
        "xbox:up": ["dpad_up", "left_y+"],
        "xbox:left_y": [{"control": "dpad_up", "value": 1}, {"control": "dpad_down", "value": -1}],
        "xbox:start": ["start"],
        "action:GBA_Jump": ["north"],
        "xbox:q": ["south"], "jump": ["south"], "xbox:x": ["nowhere", "west", 5, {"invert": true}],
        "base": 3
      },
      "switch": {"base": "gamepad", "xbox:x": ["north"]},
      "loop_a": {"base": "loop_b", "xbox:a": ["west"]}, "loop_b": {"base": "loop_a"},
      "keyboard_mouse": {"xbox:a": ["key:space"], "xbox:right_x": [{"control": "mouse:x", "sensitivity": 2.5}]},
      "bad": 7
    }
  })", &problems);
  for (const std::string& p : problems) std::printf("sys_input_tests: (expected) %s\n", p.c_str());
  // xbox:q, jump, "nowhere", 5, the object without control, base 3, the profile "bad".
  CHECK(problems.size() == 7);
  CHECK(b.profiles().size() == 5 && b.Find("gamepad") && !b.Find("bad"));
  const BindingProfile& gp = *b.Find("gamepad");
  CHECK(gp.bindings.size() == 10);  // a b lb lt right_y up left_y start, the action, x (q and jump skipped)
  const BindingProfile& km = *b.Find("keyboard_mouse");
  CHECK(km.bindings[0].sources[0].kind == BindingSource::Kind::kKey && km.bindings[0].sources[0].name == "space" &&
        km.bindings[1].sources[0].kind == BindingSource::Kind::kMouse && km.bindings[1].sources[0].sensitivity == 2.5f);

  // The identity: no file, an empty profile, or a profile binding controls to their own places.
  InputMapper none;
  GamepadState odd = Pad(0xF3FF);
  odd.left_trigger = 77, odd.thumb_lx = -32768, odd.thumb_ry = 1234;
  CHECK(none.identity() && none.Map(odd) == odd);
  CHECK(InputMapper(b, "missing").identity());
  CHECK(InputMapper(InputBindings::Parse(R"({"version":1,"profiles":{"gamepad":{"xbox:a":["south"],"xbox:left_x":["left_x"]}}})"),
                    "gamepad").identity());

  InputMapper m(b, "gamepad");
  CHECK(!m.identity());
  // A and B swapped: the south button reads B, the east button A; both, both.
  CHECK(m.Map(Pad(kA)).buttons == kB && m.Map(Pad(kB)).buttons == kA && m.Map(Pad(kA | kB)).buttons == (kA | kB));
  // LB from the left trigger, past 30, held down to 21, released at 20 (hysteresis); LT from LB: full or nothing.
  GamepadState t = Pad();
  t.left_trigger = 30;
  CHECK(!(m.Map(t).buttons & kLb));
  t.left_trigger = 31;
  CHECK(m.Map(t).buttons & kLb);
  t.left_trigger = 21;
  CHECK(m.Map(t).buttons & kLb);
  t.left_trigger = 20;
  CHECK(!(m.Map(t).buttons & kLb));
  CHECK(m.Map(t).left_trigger == 0 && m.Map(Pad(kLb)).left_trigger == 255);
  // The right stick's Y reversed (the minimum to the maximum); its X and the left X untouched.
  GamepadState s = Pad();
  s.thumb_ry = 1000, s.thumb_rx = -77, s.thumb_lx = 5;
  CHECK(m.Map(s).thumb_ry == -1000 && m.Map(s).thumb_rx == -77 && m.Map(s).thumb_lx == 5);
  s.thumb_ry = -32768;
  CHECK(m.Map(s).thumb_ry == 32767);
  // D-pad up from the D-pad or the left stick pushed up past half (0x4000), kept down to 0x3001.
  CHECK(m.Map(Pad(kUp)).buttons & kUp);
  s = Pad();
  s.thumb_ly = 0x4000;
  CHECK(!(m.Map(s).buttons & kUp));
  s.thumb_ly = 0x4001;
  CHECK(m.Map(s).buttons & kUp);
  s.thumb_ly = 0x3001;
  CHECK(m.Map(s).buttons & kUp);
  s.thumb_ly = 0x3000;
  CHECK(!(m.Map(s).buttons & kUp));
  s.thumb_ly = -0x7000;  // pushed down: not up
  CHECK(!(m.Map(s).buttons & kUp));
  // The left stick's Y from the D-pad: up full, down full the other way, both nothing; the stick itself no longer
  // moves it (the target is named, its own control not among its sources).
  CHECK(m.Map(Pad(kUp)).thumb_ly == 32767 && m.Map(Pad(kDown)).thumb_ly == -32767 && m.Map(Pad(kUp | kDown)).thumb_ly == 0);
  s = Pad();
  s.thumb_ly = 20000;
  CHECK(m.Map(s).thumb_ly == 0);
  // Start bound to itself, X left alone (its bad sources skipped, "west" kept), Y unnamed: as they are.
  CHECK(m.Map(Pad(kStart)).buttons == kStart && m.Map(Pad(kX)).buttons == kX && m.Map(Pad(0x8000)).buttons == 0x8000);
  // An action target is the adapter's: not applied here (north still Y).
  CHECK(gp.bindings[8].target == "action:GBA_Jump" && !gp.bindings[8].xbox && gp.bindings[8].sources.size() == 1);
  // The inverse, for the glyph query.
  CHECK(m.Bound(XboxControl::kA) == std::vector<PadControl>{PadControl::kEast});
  CHECK((m.Bound(XboxControl::kUp) == std::vector<PadControl>{PadControl::kDpadUp, PadControl::kLeftYPlus}));
  CHECK(m.Bound(XboxControl::kY) == std::vector<PadControl>{PadControl::kNorth});
  // A profile's base: Switch takes the gamepad's swap and binds X to the north button.
  InputMapper sw(b, "switch");
  CHECK(sw.Map(Pad(kA)).buttons == kB && sw.Map(Pad(0x8000)).buttons == (kX | 0x8000));
  // A loop of bases stops: each profile applied once.
  InputMapper loop(b, "loop_a");
  CHECK(loop.Map(Pad(kX)).buttons == (kA | kX));
  // The keyboard profile's sources are not a pad's: its targets read nothing from the pad (phase 1b maps them).
  InputMapper keys(b, "keyboard_mouse");
  CHECK(keys.Map(Pad(kA)).buttons == 0);
}

// The glyph query (docs/GLYPHS.md, section 3).
void GlyphTests() {
  using F = ControllerFamily;
  using Clock = std::chrono::steady_clock;
  // Names, sets and profiles.
  CHECK(Name(F::kXboxOne) == "xbox_one" && Name(F::kSteamDeck) == "steam_deck" && Name(F::kKeyboardMouse) == "keyboard_mouse");
  CHECK(GlyphSet(F::kGeneric) == "xbox_one" && GlyphSet(F::kPs3) == "ps3");
  CHECK(ProfileName(F::kPs4) == "playstation" && ProfileName(F::kSwitch) == "switch" && ProfileName(F::kXbox360) == "gamepad" &&
        ProfileName(F::kSteamController) == "gamepad" && ProfileName(F::kKeyboardMouse) == "keyboard_mouse");

  // Families from the host's description of a pad.
  auto family = [](PadType type, uint16_t vendor = 0, uint16_t product = 0, FaceLabel label = FaceLabel::kUnknown) {
    return FamilyOf(PadDescription{type, vendor, product, label});
  };
  CHECK(family(PadType::kXboxOne, 0x28DE, 0x1205) == F::kSteamDeck);        // SDL types the Deck as an Xbox pad
  CHECK(family(PadType::kXbox360, 0x28DE, 0x1142) == F::kSteamController);
  CHECK(family(PadType::kXbox360, 0x28DE, 0x1102) == F::kSteamController);
  CHECK(family(PadType::kXboxOne, 0x28DE, 0x11FF) == F::kXboxOne);           // Steam's virtual pad: its type decides
  CHECK(family(PadType::kXbox360, 0x045E, 0x028E) == F::kXbox360 && family(PadType::kXboxOne, 0x045E, 0x0B12) == F::kXboxOne);
  CHECK(family(PadType::kPs3) == F::kPs3 && family(PadType::kPs4) == F::kPs4 && family(PadType::kPs5) == F::kPs5);
  CHECK(family(PadType::kSwitchPro) == F::kSwitch && family(PadType::kJoyConPair) == F::kSwitch && family(PadType::kJoyConLeft) == F::kSwitch);
  CHECK(family(PadType::kUnknown, 0, 0, FaceLabel::kCross) == F::kPs4 && family(PadType::kStandard, 0, 0, FaceLabel::kB) == F::kSwitch);
  CHECK(family(PadType::kStandard, 0, 0, FaceLabel::kA) == F::kXboxOne && family(PadType::kStandard) == F::kGeneric);

  // Pads described from their SDL GUID and name, without SDL: USB GUIDs of known pads, a Bluetooth one, names alone.
  auto described = [](const char* guid, const char* name) { return FamilyOf(DescribePad(guid, name)); };
  const PadDescription deck = DescribePad("03000000de2800000512000000000000", "Steam Deck");
  CHECK(deck.vendor == 0x28DE && deck.product == 0x1205 && FamilyOf(deck) == F::kSteamDeck);
  CHECK(described("030000005e0400008e02000000000000", "Controller") == F::kXbox360);
  CHECK(described("030000005e040000120b000000000000", "Xbox Series X Controller") == F::kXboxOne);
  CHECK(described("030000004c0500006802000000000000", "PS3 Controller") == F::kPs3);
  CHECK(described("030000004c050000cc09000000000000", "PS4 Controller") == F::kPs4);
  CHECK(described("050000004c050000e60c000000000000", "DualSense Wireless Controller") == F::kPs5);  // Bluetooth
  CHECK(described("030000007e0500000920000000000000", "Nintendo Switch Pro Controller") == F::kSwitch);
  CHECK(described("03000000de2800004211000000000000", "Steam Controller") == F::kSteamController);
  CHECK(described("", "PS5 Controller") == F::kPs5 && described("", "Joy-Con (L/R)") == F::kSwitch);
  CHECK(described("", "Xbox 360 Controller") == F::kXbox360 && described("not hex at all, not hex at all!!", "Gamepad") == F::kGeneric);
  CHECK(described("03000000ffff00001234000000000000", "Unknown Gamepad") == F::kGeneric);
  // A GUID whose zero words are not zero carries no vendor (a name or a hash, not a USB id).
  CHECK(DescribePad("030000004c05abcdcc09000000000000", "").vendor == 0);

  // Real input against noise.
  GamepadState rest = Pad(), s = Pad();
  s.thumb_lx = 3000;
  CHECK(!IsRealInput(rest, s));                         // drift
  s.thumb_lx = 0x4001;
  CHECK(IsRealInput(rest, s));                          // past half range
  GamepadState s2 = s;
  s2.thumb_lx = 0x4100;
  CHECK(!IsRealInput(s, s2));                           // still past it
  s2.thumb_lx = -0x4001;
  CHECK(IsRealInput(s, s2));                            // the other side
  s = Pad(), s.left_trigger = 25;
  CHECK(!IsRealInput(rest, s));
  s.left_trigger = 31;
  constexpr uint16_t kButtonA = 0x1000;
  CHECK(IsRealInput(rest, s) && IsRealInput(rest, Pad(kButtonA)) && !IsRealInput(Pad(kButtonA), Pad(kButtonA)));

  // The active device.
  const Clock::time_point t0{};
  constexpr auto ms = [](int n) { return std::chrono::milliseconds(n); };
  ActiveDevice user;
  CHECK(user.Shown(t0) == F::kXbox360 && user.generation() == 0);  // the title's own before any device
  user.Connected(1, F::kPs5);
  CHECK(user.Shown(t0) == F::kPs5 && user.generation() == 1);      // the first device shows at once
  user.Connected(2, F::kKeyboardMouse);
  CHECK(user.Shown(t0) == F::kPs5);                                // connecting is not using
  user.Input(2, t0 + ms(1000));
  CHECK(user.Shown(t0 + ms(1100)) == F::kPs5 && user.Shown(t0 + ms(1249)) == F::kPs5);
  CHECK(user.Shown(t0 + ms(1250)) == F::kKeyboardMouse && user.generation() == 2);
  // Both hands: pad, keyboard, pad within the debounce never flicker; the pad shows 250 ms after its last take-over.
  user.Input(1, t0 + ms(2000));
  user.Input(2, t0 + ms(2100));
  user.Input(1, t0 + ms(2200));
  CHECK(user.Shown(t0 + ms(2300)) == F::kKeyboardMouse && user.Shown(t0 + ms(2449)) == F::kKeyboardMouse);
  CHECK(user.Shown(t0 + ms(2450)) == F::kPs5 && user.generation() == 3);
  // A pad plugged mid-run shows from its first input (after the debounce), not from its plugging.
  user.Connected(3, F::kSwitch);
  CHECK(user.Shown(t0 + ms(3000)) == F::kPs5);
  user.Input(3, t0 + ms(3000));
  CHECK(user.Shown(t0 + ms(3250)) == F::kSwitch);
  // Unplugged: at once to the last other device used (the DualSense, used after the keyboard), then the keyboard.
  user.Disconnected(3);
  CHECK(user.Shown(t0 + ms(3260)) == F::kPs5);
  user.Disconnected(1);
  CHECK(user.Shown(t0 + ms(3270)) == F::kKeyboardMouse);
  user.Disconnected(2);
  CHECK(user.Shown(t0 + ms(3280)) == F::kKeyboardMouse);           // none left: the last family stays
  user.Input(2, t0 + ms(3300));                                    // an unknown device's input is ignored
  CHECK(user.Shown(t0 + ms(4000)) == F::kKeyboardMouse);

  // The query.
  const InputBindings b = InputBindings::Parse(R"({"version":1,"profiles":{
    "gamepad":{"xbox:a":["east"],"xbox:b":["south"],"xbox:up":["dpad_up","left_y+"],"xbox:rt":["key:e"]},
    "playstation":{"base":"gamepad","xbox:back":["touchpad","back"]},
    "keyboard_mouse":{"xbox:a":["key:space","south"],"xbox:left_y":["key:w",{"control":"key:s","value":-1}],
                      "xbox:right_x":[{"control":"mouse:x","sensitivity":1.0}]}}})");
  GlyphQuery q(b);
  const uint32_t g0 = q.Generation(t0);
  // Before any device: the title's own Xbox 360 glyphs, through the gamepad profile.
  Glyph g = q.For(0, XboxControl::kA, t0);
  CHECK(g.family == F::kXbox360 && g.set == "xbox360" && g.control == "east");
  q.user(0).Connected(7, F::kPs4);
  CHECK(q.Generation(t0) != g0);
  g = q.For(0, XboxControl::kA, t0);
  CHECK(g.family == F::kPs4 && g.set == "ps4" && g.control == "east");      // A swapped with B, through the base
  CHECK(q.For(0, XboxControl::kBack, t0).control == "touchpad");            // the PS4 has one
  CHECK(q.For(0, XboxControl::kY, t0).control == "north");                  // unnamed: its own place
  CHECK(q.For(0, XboxControl::kUp, t0).control == "dpad_up");               // the first of two sources
  CHECK(q.For(0, XboxControl::kRt, t0).control.empty());                    // only a key bound: nothing on a pad
  CHECK(q.For(0, XboxControl::kLeftX, t0).control == "left_x");
  q.user(0).Connected(8, F::kPs3);
  q.user(0).Input(8, t0);
  CHECK(q.For(0, XboxControl::kBack, t0 + ms(250)).control == "back");      // no touchpad on a PS3: the next source
  // A family whose profile the file lacks falls back to the gamepad's.
  q.user(1).Connected(9, F::kSwitch);
  g = q.For(1, XboxControl::kA, t0);
  CHECK(g.set == "switch" && g.control == "east");
  q.user(1).Connected(10, F::kGeneric);
  q.user(1).Input(10, t0);
  CHECK(q.For(1, XboxControl::kB, t0 + ms(250)).set == "xbox_one");
  // The keyboard: its first key or mouse control; nothing when the profile names none.
  q.user(2).Connected(11, F::kKeyboardMouse);
  CHECK(q.For(2, XboxControl::kA, t0).control == "key:space" && q.For(2, XboxControl::kLeftY, t0).control == "key:w");
  CHECK(q.For(2, XboxControl::kRightX, t0).control == "mouse:x" && q.For(2, XboxControl::kB, t0).control.empty());
  // A player's own layout ("player<n>") wins over their pad family's, for the input and the glyphs alike.
  const InputBindings players = InputBindings::Parse(R"({"version":1,"profiles":{
    "gamepad":{"xbox:a":["east"],"xbox:b":["south"]},
    "playstation":{"base":"gamepad","xbox:y":["west"]},
    "player2":{"base":"gamepad","xbox:a":["north"]}}})");
  CHECK(ProfileFor(players, 0, F::kPs5) == "playstation" && ProfileFor(players, 1, F::kPs5) == "player2");
  CHECK(ProfileFor(players, 1, F::kKeyboardMouse) == "keyboard_mouse" && ProfileFor(players, 2, F::kCount) == "gamepad");
  CHECK(ProfileFor(players, 0, F::kXboxOne) == "gamepad" && ProfileFor(InputBindings{}, 3, F::kSwitch) == "switch");
  GlyphQuery pq(players);
  pq.user(0).Connected(1, F::kPs5);
  pq.user(1).Connected(2, F::kPs5);
  CHECK(pq.For(0, XboxControl::kA, t0).control == "east" && pq.For(0, XboxControl::kY, t0).control == "west");
  CHECK(pq.For(1, XboxControl::kA, t0).control == "north" && pq.For(1, XboxControl::kB, t0).control == "south");
  CHECK(pq.For(1, XboxControl::kY, t0).control == "north");  // player 2's layout does not take the family's Y
  InputMapper p1(players, ProfileFor(players, 0, F::kPs5), "gamepad"), p2(players, ProfileFor(players, 1, F::kPs5), "gamepad");
  CHECK(p1.Bound(XboxControl::kA) == std::vector<PadControl>{PadControl::kEast} &&
        p2.Bound(XboxControl::kA) == std::vector<PadControl>{PadControl::kNorth});
  // New bindings: the answers follow at once, the generation moves.
  const uint32_t g1 = q.Generation(t0 + ms(300));
  q.SetBindings(InputBindings{});
  CHECK(q.Generation(t0 + ms(300)) != g1 && q.For(0, XboxControl::kA, t0 + ms(300)).control == "south");
  CHECK(q.For(2, XboxControl::kA, t0).control.empty());
  // An answer allocates nothing.
  q.SetBindings(b);
  const AllocCounts before = ThreadAllocations();
  size_t total = 0;
  for (int i = 0; i < 1000; ++i) total += q.For(uint32_t(i & 3), XboxControl(i % int(XboxControl::kCount)), t0 + ms(i)).control.size();
  const AllocCounts spent = ThreadAllocations() - before;
  CHECK(spent.count == 0 && spent.bytes == 0 && total > 0);
}

// Several players: the user slots, and each player's glyphs following their own pad.
void PlayerTests() {
  constexpr uint32_t kNone = PlayerSlots::kNone;
  PlayerSlots p;
  CHECK(!p.Connected(0) && p.UserOf(1) == kNone);
  // Split screen: two pads, players 1 and 2 (users 0 and 1); the keyboard joins player 1 without a slot.
  CHECK(p.ConnectPad(10, 0xA) == 0 && p.ConnectPad(11, 0xB) == 1 && p.ConnectKeyboard(50) == 0);
  CHECK(p.ConnectPad(10, 0xA) == 0);  // connected twice: the same slot
  CHECK(p.Connected(0) && p.Connected(1) && !p.Connected(2) && p.UserOf(50) == 0 && p.PadIn(1) == 11);
  // Player 1's pad unplugged: player 2 stays user 1, slot 0 waits for its pad; the keyboard keeps player 1 connected.
  CHECK(p.Disconnected(10) == kNone);
  CHECK(p.UserOf(11) == 1 && p.PadIn(0) == kNone && p.Connected(0));
  // Another pad plugged meanwhile does not take player 1's place; the first pad back takes it again (a new
  // connection, same identity).
  CHECK(p.ConnectPad(12, 0xC) == 2);
  CHECK(p.ConnectPad(13, 0xA) == 0 && p.UserOf(11) == 1);
  // Four pads in; a fifth waits, then takes the slot a pad leaves.
  CHECK(p.ConnectPad(14, 0xD) == 3 && p.ConnectPad(15, 0xE) == kNone && p.UserOf(15) == kNone);
  CHECK(p.Disconnected(12) == 15 && p.UserOf(15) == 2);
  // All reservations taken by others: a new pad takes the oldest one.
  PlayerSlots q;
  for (uint32_t d = 0; d < 4; ++d) CHECK(q.ConnectPad(d, 100 + d) == d);
  q.Disconnected(2);
  q.Disconnected(0);
  CHECK(q.ConnectPad(9, 999) == 2 && q.ConnectPad(8, 998) == 0);
  // A waiting pad unplugged leaves the queue; an unknown device changes nothing.
  CHECK(q.ConnectPad(7, 997) == kNone && q.Disconnected(7) == kNone && q.Disconnected(1234) == kNone);
  q.Disconnected(1);
  CHECK(q.PadIn(1) == kNone && q.ConnectPad(1, 101) == 1);
  // The keyboard's player can be set.
  PlayerSlots k;
  k.set_keyboard_user(1);
  CHECK(k.ConnectKeyboard(50) == 1 && k.Connected(1) && !k.Connected(0));
  CHECK(k.Disconnected(50) == kNone && !k.Connected(1));

  // Each player's glyphs follow their own devices: player 2 on a DualSense, player 1 on an Xbox pad and the keyboard.
  using F = ControllerFamily;
  const auto t0 = std::chrono::steady_clock::time_point{};
  GlyphQuery glyphs(InputBindings::Parse(R"({"version":1,"profiles":{"keyboard_mouse":{"xbox:a":["key:space"]}}})"));
  PlayerSlots slots;
  struct Device {
    uint32_t id;
    uint64_t identity;
    F family;
  };
  for (const Device& d : {Device{20, 0x1, F::kXboxOne}, Device{21, 0x2, F::kPs5}}) {
    const uint32_t user = slots.ConnectPad(d.id, d.identity);
    glyphs.user(user).Connected(d.id, d.family);
  }
  glyphs.user(slots.ConnectKeyboard(60)).Connected(60, F::kKeyboardMouse);
  CHECK(glyphs.For(0, XboxControl::kA, t0).set == "xbox_one" && glyphs.For(1, XboxControl::kA, t0).set == "ps5");
  // Player 1 types: only player 1's prompts turn to keys.
  glyphs.user(slots.UserOf(60)).Input(60, t0);
  const auto later = t0 + std::chrono::milliseconds(300);
  CHECK(glyphs.For(0, XboxControl::kA, later).control == "key:space" && glyphs.For(1, XboxControl::kA, later).control == "south");
  CHECK(glyphs.For(1, XboxControl::kA, later).family == F::kPs5);
  // Player 2 presses: player 1's prompts stay on the keyboard.
  glyphs.user(slots.UserOf(21)).Input(21, later);
  CHECK(glyphs.For(0, XboxControl::kA, later + std::chrono::milliseconds(300)).family == F::kKeyboardMouse);
}

// Every glyph name the query can answer is in the family's committed glyph set (assets/glyphs/<set>.json).
void GlyphSetTests() {
  for (size_t f = 0; f < size_t(ControllerFamily::kCount); ++f) {
    const auto family = ControllerFamily(f);
    const std::string path = std::string(ANY360_GLYPHS_DIR) + "/" + std::string(GlyphSet(family)) + ".json";
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    if (!file) continue;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    json::Value v;
    CHECK(json::Parse(text, v) && v.Find("controls"));
    const json::Value* controls = v.Find("controls");
    if (!controls) continue;
    auto has = [&](std::string_view name) {
      if (controls->Find(std::string(name))) return true;
      std::printf("sys_input_tests: %s has no glyph %.*s\n", path.c_str(), int(name.size()), name.data());
      return false;
    };
    if (family == ControllerFamily::kKeyboardMouse) {
      for (const char* name : {"key:space", "key:w", "key:page_up", "key:numpad_enter", "key:equals", "mouse:left",
                               "mouse:x", "mouse:wheel_up", "key:wasd"})
        CHECK(has(name));
      continue;
    }
    // Every pad control (the touchpad on the families that have one), as the query names them.
    const bool touchpad = family == ControllerFamily::kPs4 || family == ControllerFamily::kPs5;
    for (size_t c = 0; c < size_t(PadControl::kCount); ++c)
      if (PadControl(c) != PadControl::kTouchpad || touchpad) CHECK(has(Name(PadControl(c))));
  }
}

// Any360's keyboard layout under the file's (WithDefaultKeyboard), and the keyboard's glyphs from it.
void DefaultKeyboardTests() {
  using F = ControllerFamily;
  const auto now = GlyphQuery::Clock::now();
  // No file: Any360's layout, every control named; the left stick one W A S D glyph, the right stick the mouse.
  const InputBindings none = InputBindings{}.WithDefaultKeyboard();
  const auto keys = ResolveProfile(none, "keyboard_mouse");
  uint32_t named = 0;
  for (const Binding* b : keys) named += b != nullptr;
  CHECK(named == uint32_t(XboxControl::kCount));
  GlyphQuery q(none);
  q.user(0).Connected(1, F::kKeyboardMouse);
  CHECK(q.For(0, XboxControl::kA, now).control == "key:space" && q.For(0, XboxControl::kB, now).control == "key:control");
  CHECK(q.For(0, XboxControl::kLt, now).control == "mouse:right" && q.For(0, XboxControl::kStart, now).control == "key:escape");
  CHECK(q.For(0, XboxControl::kLeftX, now).control == "key:wasd" && q.For(0, XboxControl::kLeftY, now).control == "key:wasd");
  CHECK(q.For(0, XboxControl::kRightX, now).control == "mouse:x" && q.For(0, XboxControl::kUp, now).control == "key:1");
  // A pad is not touched.
  q.user(1).Connected(2, F::kXboxOne);
  CHECK(q.For(1, XboxControl::kA, now).control == "south");
  // The file's keyboard profile wins where it names a control, through its own base chain; Any360's below it.
  const InputBindings file = InputBindings::Parse(R"({"version":1,"profiles":{
    "shared":{"xbox:y":["key:p"]},"keyboard_mouse":{"base":"shared","xbox:a":["key:enter"],
    "xbox:left_x":["key:right",{"control":"key:left","value":-1}],"xbox:left_y":["key:up",{"control":"key:down","value":-1}]}}})");
  const InputBindings layered = file.WithDefaultKeyboard();
  CHECK(layered.Find("shared")->base == "any360_keyboard" && layered.Find("keyboard_mouse")->base == "shared");
  CHECK(layered.WithDefaultKeyboard().profiles().size() == layered.profiles().size());  // once
  GlyphQuery r(layered);
  r.user(0).Connected(1, F::kKeyboardMouse);
  CHECK(r.For(0, XboxControl::kA, now).control == "key:enter" && r.For(0, XboxControl::kY, now).control == "key:p");
  CHECK(r.For(0, XboxControl::kB, now).control == "key:control");
  CHECK(r.For(0, XboxControl::kLeftX, now).control == "key:arrows");
}
void Bench() {
  const InputBindings b = InputBindings::Parse(R"({"version":1,"profiles":{"gamepad":{
    "xbox:a":["east"],"xbox:b":["south"],"xbox:x":["north"],"xbox:y":["west"],"xbox:lb":["left_trigger"],
    "xbox:lt":["left_shoulder"],"xbox:right_y":[{"control":"right_y","invert":true}],"xbox:up":["dpad_up","left_y+"]}}})");
  InputMapper identity, mapped(b, "gamepad");
  GamepadState s = Pad(0x3101);
  s.left_trigger = 100, s.thumb_ly = 20000, s.thumb_ry = -5000;
  constexpr int kCalls = 10'000'000;
  for (InputMapper* m : {&identity, &mapped}) {
    uint32_t sink = 0;
    const AllocCounts before = ThreadAllocations();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) {
      s.left_trigger = uint8_t(i);
      sink += m->Map(s).buttons;
    }
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
    const AllocCounts spent = ThreadAllocations() - before;
    std::printf("sys_input_tests: Map, %s: %.2f ns a sample, %.3f allocations and %.1f bytes a sample (%u)\n",
                m == &identity ? "no bindings" : "8 targets bound", ns, double(spent.count) / kCalls,
                double(spent.bytes) / kCalls, sink & 1);
  }

  // The glyph query: an answer, a reported input, a family from a description, a real-input test.
  GlyphQuery q(b);
  q.user(0).Connected(1, ControllerFamily::kPs5);
  q.user(0).Connected(2, ControllerFamily::kKeyboardMouse);
  const auto t0 = std::chrono::steady_clock::time_point{};
  auto measure = [&](const char* what, auto&& call) {
    size_t sink = 0;
    const AllocCounts before = ThreadAllocations();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) sink += call(i);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
    const AllocCounts spent = ThreadAllocations() - before;
    std::printf("sys_input_tests: %s: %.2f ns a call, %.3f allocations and %.1f bytes a call (%zu)\n", what, ns,
                double(spent.count) / kCalls, double(spent.bytes) / kCalls, sink & 1);
  };
  measure("GlyphQuery::For", [&](int i) {
    return q.For(0, XboxControl(i % int(XboxControl::kCount)), t0 + std::chrono::microseconds(i)).control.size();
  });
  measure("ActiveDevice::Input + Shown, two devices alternating", [&](int i) {
    const auto at = t0 + std::chrono::microseconds(i);
    q.user(0).Input(1 + uint32_t((i >> 10) & 1), at);
    return size_t(q.user(0).Shown(at));
  });
  measure("FamilyOf", [&](int i) {
    return size_t(FamilyOf(PadDescription{PadType(i % 11), uint16_t(i & 1 ? 0x28DE : 0x045E), uint16_t(0x1100 + (i & 0x1FF)),
                                          FaceLabel(i % 9)}));
  });
  PlayerSlots slots;
  for (uint32_t d = 0; d < 3; ++d) slots.ConnectPad(d, 100 + d);
  measure("PlayerSlots::ConnectPad + Disconnected (a pad unplugged and back)", [&](int) {
    slots.Disconnected(1);
    return size_t(slots.ConnectPad(1, 101));
  });
  measure("PlayerSlots::UserOf", [&](int i) { return size_t(slots.UserOf(uint32_t(i & 3))); });
  GamepadState a = Pad(), c = Pad();
  measure("IsRealInput", [&](int i) {
    c.thumb_lx = int16_t(i * 7), c.left_trigger = uint8_t(i);
    return size_t(IsRealInput(a, c));
  });
}

}  // namespace

int main(int argc, char** argv) {
  JsonTests();
  BindingsTests();
  GlyphTests();
  GlyphSetTests();
  PlayerTests();
  DefaultKeyboardTests();
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
  std::printf("sys_input_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
