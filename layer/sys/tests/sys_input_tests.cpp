// The input bindings (docs/GLYPHS.md, section 2): the JSON reader, the bindings file and layer 1's mapper, against
// the documented behaviour; then a sample's cost with and without bindings (time and host allocations a call).
//   sys_input_tests [--bench]
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "any360/sys/alloc_meter.h"
#include "any360/sys/input_bindings.h"
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
}

}  // namespace

int main(int argc, char** argv) {
  JsonTests();
  BindingsTests();
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) Bench();
  std::printf("sys_input_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
