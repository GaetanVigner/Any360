// Any360's keys (any360/host/hotkeys.h): one key each, none of the runtime's own, settings named bind_any360_*, and the
// numbered files the screenshot and capture keys write to. The binds themselves are the runtime's
// (rex::ui::RegisterBind); what they do is checked in game.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "any360/host/hotkeys.h"

using namespace any360::host::rexglue;

namespace {
int g_checks = 0, g_failures = 0;
#define CHECK(condition)                                                                                  \
  do {                                                                                                    \
    ++g_checks;                                                                                           \
    if (!(condition) && ++g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  } while (0)
}  // namespace

int main() {
  std::set<std::string> keys, binds;
  for (const Hotkey& key : kHotkeys) {
    CHECK(keys.insert(key.default_key).second);  // one action a key
    CHECK(binds.insert(key.bind).second);
    CHECK(std::strncmp(key.bind, "bind_any360_", 12) == 0);
    CHECK(std::strncmp(key.description, "Any360: ", 8) == 0);
    for (const char* runtime : kRuntimeKeys) CHECK(std::strcmp(key.default_key, runtime) != 0);
  }
  CHECK(keys.size() == 4);
  CHECK(std::string(kFrameCounterKey.default_key) == "F2" && std::string(kScreenshotKey.default_key) == "F5" &&
        std::string(kProfileKey.default_key) == "F6" && std::string(kGpuCaptureKey.default_key) == "F8");

  // Numbered files: the folder made, the first free number taken, gaps filled from the start.
  const std::filesystem::path folder = std::filesystem::temp_directory_path() / "any360_hotkeys_test";
  std::error_code error;
  std::filesystem::remove_all(folder, error);
  const std::filesystem::path first = NextNumberedPath(folder / "shots", "any360_", ".png");
  CHECK(std::filesystem::is_directory(folder / "shots"));
  CHECK(first == folder / "shots" / "any360_1.png");
  std::ofstream(first).put('x');
  CHECK(NextNumberedPath(folder / "shots", "any360_", ".png") == folder / "shots" / "any360_2.png");
  std::ofstream(folder / "shots" / "any360_2.png").put('x');
  std::ofstream(folder / "shots" / "any360_4.png").put('x');
  CHECK(NextNumberedPath(folder / "shots", "any360_", ".png") == folder / "shots" / "any360_3.png");
  CHECK(NextNumberedPath(folder / "shots", "any360_", ".a360cap") == folder / "shots" / "any360_1.a360cap");
  std::filesystem::remove_all(folder, error);

  std::printf("host_hotkeys_tests: %d checks, %d failing checks\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
