// Any360's own keys, the same in every title on the binding (registered with the runtime's key binds, so each is a
// setting, bind_any360_*, the player can move to another key; the title never sees a bound key):
//   F2  the frame counter shown or hidden (--any360_fps_overlay sets it at the start)
//   F5  the frame as rendered, to <user data>/screenshots/any360_<n>.png
//   F6  every thread sampled for any360_profile_seconds (10 when 0), profile.txt / profile.csv in the logs
//   F8  the next any360_gpu_capture_kicks kicks of the GPU recorded, to any360_gpu_capture or
//       <user data>/captures/any360_<n>.a360cap
//       (the title's data: it stays on the machine)
// The runtime binds F3, F4, F7 and Backtick itself (its overlays); titles keep the rest.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace any360::host::rexglue {

struct Hotkey {
  const char* bind;         // the setting holding its key
  const char* default_key;  // a key name the runtime parses (rex::ui::ParseVirtualKey)
  const char* description;
};
inline constexpr Hotkey kFrameCounterKey{"bind_any360_fps_overlay", "F2", "Any360: show or hide the frame counter"};
inline constexpr Hotkey kScreenshotKey{"bind_any360_screenshot", "F5",
                                       "Any360: the frame as rendered, to <user data>/screenshots/any360_<n>.png"};
inline constexpr Hotkey kProfileKey{"bind_any360_profile", "F6",
                                    "Any360: sample every thread for any360_profile_seconds (10 when 0) into the logs"};
inline constexpr Hotkey kGpuCaptureKey{"bind_any360_gpu_capture", "F8",
                                       "Any360: record the GPU's next kicks to <user data>/captures/any360_<n>.a360cap (title data)"};
inline constexpr Hotkey kHotkeys[] = {kFrameCounterKey, kScreenshotKey, kProfileKey, kGpuCaptureKey};
// The keys the runtime binds itself (its debug, settings, achievements and console overlays).
inline constexpr const char* kRuntimeKeys[] = {"F3", "F4", "F7", "Backtick"};

// Where a key writes its files: `name` (screenshots, captures) under the title's user data folder
// (--user_data_root, the runtime's user_data_root()), which every system lets the player write and the launcher knows
// (Any360Launcher's Screenshots module reads <user data>/screenshots). Not the working directory: it depends on how the
// title was started, and an installed title's folder (Program Files, /usr) cannot be written. Without user data (tool
// mode) the name alone, relative to the working directory, as before.
inline std::filesystem::path TitleDataFolder(const std::filesystem::path& user_data_root, std::string_view name) {
  return user_data_root.empty() ? std::filesystem::path(name) : user_data_root / name;
}

// `folder`/`stem`<n>`extension` for the first n from 1 with no such file yet, the folder made if missing.
inline std::filesystem::path NextNumberedPath(const std::filesystem::path& folder, std::string_view stem,
                                              std::string_view extension) {
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  for (unsigned n = 1;; ++n) {
    std::filesystem::path path = folder / (std::string(stem) + std::to_string(n) + std::string(extension));
    if (!std::filesystem::exists(path, error)) return path;
  }
}

}  // namespace any360::host::rexglue
