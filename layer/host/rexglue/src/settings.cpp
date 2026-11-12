#include "any360/host/settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <sstream>

#include "any360/sys/files.h"
#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"

REXCVAR_DEFINE_STRING(any360_sys_groups, "", "Kernel",
                      "Any360: system call groups run on any360::sys, comma-separated (stateless, files, objects, threads, "
                      "memory, xam, input, audio), all (the eight) or none; empty: the title's default (none unless "
                      "the title sets one)");
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
REXCVAR_DEFINE_INT32(any360_input_sample_us, 1000, "Kernel",
                     "Any360 input group: the controllers sampled on their own thread every this many microseconds, every "
                     "change queued for the title's polls; 0: sampled at each poll");
REXCVAR_DEFINE_BOOL(any360_sys_keep_files, true, "Kernel",
                    "Any360: an open file keeps its host file open between its reads; off: every read opens it "
                    "(to tell whether a title's trouble comes from that)");
REXCVAR_DEFINE_STRING(any360_sys_network, "loopback", "Kernel",
                      "Any360: the console's network with the xam group: loopback (the console alone at 127.0.0.1, its "
                      "sockets the host's, nothing leaves the machine) or off (no connection: every socket call fails)");
REXCVAR_DEFINE_BOOL(any360_sys_log_files, false, "Kernel",
                    "Any360: every file the title opens and closes and every read that gives fewer bytes than asked, by "
                    "guest path, in the log");
REXCVAR_DEFINE_INT32(any360_sys_wait_look_ms, 16, "Kernel",
                     "Any360: the longest a sleeping wait goes without looking at its objects again, in milliseconds. "
                     "A wait is woken by signals, timers and APCs; looking only finds a signal state written straight "
                     "into guest memory, which no title at hand does. 1 (with any360_sys_wait_first_look_ms=1): a look "
                     "every millisecond, as the layer first did");
REXCVAR_DEFINE_INT32(any360_sys_wait_first_look_ms, 16, "Kernel",
                     "Any360: how long a sleeping wait goes before its first look at its objects (each next one twice "
                     "as late, up to any360_sys_wait_look_ms)");
REXCVAR_DEFINE_INT32(any360_input_step_us, 16683, "Kernel",
                     "Any360 input group: polls step through the queued changes at most this often (one console frame), so "
                     "each stays visible for a frame; 0: every poll steps");

namespace any360::host::rexglue {

void InstallCrashReport();  // crash_report.cpp

namespace {

bool g_started_from_settings = false;
std::string g_default_groups;

constexpr const char* kAllGroups = "stateless,files,objects,threads,memory,xam,input,audio";

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

void SetDefaultSystemGroups(const std::string& groups) { g_default_groups = groups; }

std::vector<std::string> SystemGroupsNamed(const std::string& setting, const std::string& title_default) {
  std::vector<std::string> groups;
  for (const std::string& name : SplitList(setting.empty() ? title_default : setting)) {
    if (name == "none") continue;
    if (name == "all") {
      for (const std::string& each : SplitList(kAllGroups)) groups.push_back(each);
      continue;
    }
    groups.push_back(name);
  }
  std::vector<std::string> unique;
  for (const std::string& name : groups) {
    if (std::find(unique.begin(), unique.end(), name) == unique.end()) unique.push_back(name);
  }
  return unique;
}

bool StartSystemFromSettings(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root,
                             const std::vector<SystemMount>& title_mounts) {
  const std::vector<std::string> groups = SystemGroupsNamed(REXCVAR_GET(any360_sys_groups), g_default_groups);
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
    if (group == SysGroup::kObjects) {
      any360::sys::Objects().SetWaitLook(std::chrono::milliseconds(std::max(1, REXCVAR_GET(any360_sys_wait_look_ms))),
                                         std::chrono::milliseconds(std::max(1, REXCVAR_GET(any360_sys_wait_first_look_ms))));
      any360::sys::Objects().SetWaitLog([](const std::string& line) { REXLOG_INFO("Any360 system: {}", line); });
    }
    if (group == SysGroup::kFiles) {
      any360::sys::Files().SetKeepHostFiles(REXCVAR_GET(any360_sys_keep_files));
      any360::sys::Files().SetLog([](const std::string& line) { REXLOG_INFO("Any360 system: {}", line); },
                                  REXCVAR_GET(any360_sys_log_files));
    }
    if (group == SysGroup::kXam) {
      const std::string network = REXCVAR_GET(any360_sys_network);
      any360::sys::Net().SetMode(network == "off" ? any360::sys::Network::Mode::kOff : any360::sys::Network::Mode::kLoopback);
      if (network != "off" && network != "loopback") REXLOG_WARN("Any360 system: no network mode '{}': loopback", network);
    }
    if (group == SysGroup::kInput) {
      any360::sys::XamState().SetInputStep(std::chrono::microseconds(std::max(0, REXCVAR_GET(any360_input_step_us))));
      if (REXCVAR_GET(any360_input_sample_us) > 0) {
        any360::sys::XamState().StartInputSampling(std::chrono::microseconds(REXCVAR_GET(any360_input_sample_us)));
      }
    }
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
  any360::sys::XamState().StopInputSampling();
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
