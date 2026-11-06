#include "any360/host/settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <sstream>

#include "any360/sys/files.h"
#include "any360/sys/scheduler.h"

REXCVAR_DEFINE_STRING(any360_sys_groups, "", "Kernel",
                      "Any360: system call groups run on any360::sys, comma-separated (stateless, files, objects, threads, "
                      "memory, xam, input, audio); empty: all on the runtime");
REXCVAR_DEFINE_BOOL(any360_sys_trace, false, "Kernel",
                    "Any360: log the first call of each system call that goes to any360::sys, and to the runtime");
REXCVAR_DEFINE_BOOL(any360_sys_trace_all, false, "Kernel",
                    "Any360: log every system call that goes to any360::sys, with its arguments and result (slow)");
REXCVAR_DEFINE_STRING(any360_sys_trace_groups, "", "Kernel",
                      "Any360: the groups any360_sys_trace_all reports, comma-separated; empty: all");
REXCVAR_DEFINE_STRING(any360_sys_trace_names, "", "Kernel",
                      "Any360: the calls any360_sys_trace_all reports, comma-separated; empty: all");
REXCVAR_DEFINE_STRING(any360_sys_runtime_calls, "", "Kernel",
                      "Any360: system calls left to the runtime though their group is on, comma-separated (debugging)");

namespace any360::host::rexglue {

void InstallCrashReport();  // crash_report.cpp

namespace {

bool g_started_from_settings = false;

std::vector<std::string> SplitList(const std::string& text) {
  std::vector<std::string> items;
  std::stringstream list(text);
  std::string item;
  while (std::getline(list, item, ',')) {
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty()) items.push_back(item);
  }
  return items;
}

}  // namespace

bool StartSystemFromSettings(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root,
                             const std::vector<SystemMount>& title_mounts) {
  const std::vector<std::string> groups = SplitList(REXCVAR_GET(any360_sys_groups));
  if (groups.empty()) return false;
  SystemOptions options;
  options.trace_calls = REXCVAR_GET(any360_sys_trace);
  options.trace_every_call = REXCVAR_GET(any360_sys_trace_all);
  options.log = [](const std::string& line) { REXLOG_INFO("Any360 system: {}", line); };
  for (const std::string& name : SplitList(REXCVAR_GET(any360_sys_trace_groups))) {
    SysGroup group;
    if (ParseSysGroup(name, group)) options.trace_groups.push_back(group);
  }
  options.trace_names = SplitList(REXCVAR_GET(any360_sys_trace_names));
  options.runtime_calls = SplitList(REXCVAR_GET(any360_sys_runtime_calls));
  // The title's files as the runtime mounts them (Runtime::SetupVfs), then the title's own devices.
  const bool game_writes = rex::cvar::GetFlagByName("allow_game_relative_writes") == "true";
  options.mounts.push_back({"\\Device\\Harddisk0\\Partition1", std::filesystem::absolute(game_data_root).string(),
                            !game_writes, {"game:", "d:"}});
  options.mounts.insert(options.mounts.end(), title_mounts.begin(), title_mounts.end());
  options.user_data_root = std::filesystem::absolute(user_data_root).string();
  std::string error;
  InstallCrashReport();  // a crash of a title without its own reporter still says where
  if (!StartSystem(options, error)) {
    REXLOG_ERROR("Any360 system: {} (every system call stays on the runtime)", error);
    return false;
  }
  std::string switched;
  for (const std::string& name : groups) {
    SysGroup group;
    if (!ParseSysGroup(name, group)) {
      REXLOG_WARN("Any360 system: no group named '{}'", name);
      continue;
    }
    if (group == SysGroup::kAudio) {
#ifdef ANY360_REXGLUE_AUDIO
      // The decoder and the output first: the audio calls need them.
      if (!StartAudio(error)) {
        REXLOG_ERROR("Any360 system: audio stays on the runtime: {}", error);
        continue;
      }
#else
      REXLOG_ERROR("Any360 system: audio stays on the runtime: built without the XMA decoder (FFmpeg's headers)");
      continue;
#endif
    }
    EnableSystemGroup(group, true);
    switched += (switched.empty() ? "" : ", ") + name;
  }
  const uint32_t variables = BindVariables();
  REXLOG_INFO("Any360 system: {} on any360::sys ({} variable imports)", switched.empty() ? "no group" : switched, variables);
  g_started_from_settings = true;
  return true;
}

void StopSystemFromSettings() {
  if (!g_started_from_settings) return;
#ifdef ANY360_REXGLUE_AUDIO
  StopAudio();
#endif
  // The title's threads park in their next system call or wait: the runtime frees the guest memory next.
  any360::sys::BeginShutdown();
}

void MountFolderIfStarted(const std::string& guest_folder, const std::filesystem::path& host) {
  if (!g_started_from_settings) return;
  // The longest mounted prefix wins, so this folder shadows the title's own under the partition's mount.
  any360::sys::Files().Mount(guest_folder, host, /*read_only=*/true);
  REXLOG_INFO("Any360 system: {} mounted at {}", guest_folder, host.string());
}

}  // namespace any360::host::rexglue
