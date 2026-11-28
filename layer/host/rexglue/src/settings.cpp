#include "any360/host/settings.h"
#include "any360/host/players.h"
#include "any360/host/keyboard_binds.h"
#include "any360/host/usage.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

#include "any360/sys/files.h"
#include "any360/sys/net.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"

REXCVAR_DEFINE_STRING(any360_cpu_affinity, "", "Kernel",
                      "Any360: the logical processors the title runs on, as a mask (0xFF) or a list (0-7, 0,1): set when "
                      "the system binding starts, after the runtime's own processor setup, and set again by each frame "
                      "report that finds it changed (the report's usage says the mask); empty: the system's choice");
REXCVAR_DEFINE_STRING(any360_sys_groups, "", "Kernel",
                      "Any360: system call groups run on any360::sys, comma-separated (stateless, files, objects, threads, "
                      "memory, xam, input, audio), all (the eight) or none; empty: the title's default (none unless "
                      "the title sets one)");
REXCVAR_DEFINE_INT32(any360_hitch_trace_ms, 0, "GPU",
                     "Any360: a frame of play longer than this many milliseconds has every thread's stack sampled until "
                     "it ends, and hitch_<n>.txt says what each thread did meanwhile (any360/host/hitch_tracer.h); 0: off");
REXCVAR_DEFINE_BOOL(any360_fps_overlay, false, "GPU",
                    "Any360: a frame counter drawn by Any360 over the title's picture, top left (frames per second, frame "
                    "time and its slowest, median and 10% / 1% lows, GPU time a frame, command processor idle share); "
                    "every title on any360::gfx, nothing done when off");
REXCVAR_DEFINE_STRING(any360_hitch_trace_folder, "logs", "GPU", "Any360: the folder the hitch tracer's reports go to");
REXCVAR_DEFINE_INT32(any360_profile_seconds, 0, "GPU",
                     "Any360: samples where every thread spends its processor time for this many seconds and writes "
                     "profile.txt and profile.csv, function by function (any360/host/profiler.h); 0: off");
REXCVAR_DEFINE_INT32(any360_profile_after, 60, "GPU",
                     "Any360: the seconds after the graphics start the profiler waits before it samples");
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
REXCVAR_DEFINE_INT32(any360_sys_default_stack_kb, 0, "Kernel",
                     "Any360 threads group: the stack of a thread the title creates with no size, in KB; 0: the title's "
                     "default (its XEX header's, as the console). Smaller values reproduce a stack overflow sooner");
REXCVAR_DEFINE_BOOL(any360_sys_log_threads, false, "Kernel",
                    "Any360 threads group: each thread the title creates (start address, stack) in the log. Threads "
                    "that use more than 3/4 of their stack or run past its end are logged either way");
REXCVAR_DEFINE_BOOL(any360_sys_spin_pacing, true, "Kernel",
                    "Any360 threads group: a thread whose Sleep(0) loop waits at a steady cadence (a frame limiter) "
                    "sleeps until shortly before its wait's predicted end instead of spinning a processor all through "
                    "it (any360/sys/spin_pacer.h); off: every Sleep(0) yields");
REXCVAR_DEFINE_BOOL(any360_sys_idle_backoff, true, "Kernel",
                    "Any360 threads group, with any360_sys_spin_pacing: a thread whose Sleep(0) waits have no cadence "
                    "(polling memory for work, as a render thread waits for its commands) sleeps in slices once a wait "
                    "has gone on for 0.2 ms (1/8 of what it has waited, 100-500 us asked: 0.5-1 ms with Windows' timer) "
                    "instead of yielding; the work is seen at most a slice later");
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
REXCVAR_DEFINE_STRING(any360_input_script, "", "Kernel",
                      "Any360: button presses played on the first controller, for tests without a player: "
                      "\"20:start,24:a/500,30:a+x,40:rt/20000\" (seconds:buttons[/ms held], from the start; lt and rt pull a "
                      "trigger all the way)");
REXCVAR_DEFINE_STRING(any360_input_record, "", "Kernel",
                      "Any360 input group: records the first controller (buttons, triggers, sticks, every change) to this "
                      "file from any360_input_record_from seconds after the start, for any360_input_record_seconds (0: "
                      "until the title ends); written as it goes (any360/sys/input.h, InputRecording)");
REXCVAR_DEFINE_STRING(any360_input_replay, "", "Kernel",
                      "Any360 input group: plays a recording (any360_input_record) back on the first controller from "
                      "any360_input_record_from seconds after the start, the controller itself left out; with the same "
                      "input script both runs reach the same point first: an A/B on a player's route without a player");
REXCVAR_DEFINE_DOUBLE(any360_input_record_from, 0.0, "Kernel",
                      "Any360: the seconds after the start a recording or a replay begins (the input script's clock)");
REXCVAR_DEFINE_DOUBLE(any360_input_record_seconds, 0.0, "Kernel",
                      "Any360: how long a recording lasts (a neutral state closes it); 0: until the title ends");
REXCVAR_DEFINE_BOOL(any360_input_players, true, "Kernel",
                   "Any360 input group: Any360 decides which device is which player (stable split-screen slots, each "
                   "pad's family for its bindings profile and glyphs); false: the runtime's own assignment");
REXCVAR_DEFINE_STRING(any360_input_bindings, "", "Kernel",
                      "Any360 input group: the bindings file Any360Launcher writes (docs/GLYPHS.md, section 2); its "
                      "\"gamepad\" profile maps every controller before the title reads it. Empty: no remapping");
REXCVAR_DEFINE_INT32(any360_keyboard_player, -1, "Kernel",
                     "Any360 input group: the player the keyboard and mouse play (1 to 4; 0: no keyboard player, the "
                     "runtime's keyboard driver off); -1: the runtime's own setting (mnk_mode), on the first player");
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
  if (const std::string affinity = REXCVAR_GET(any360_cpu_affinity); !affinity.empty()) {
    const uint64_t mask = ParseAffinityMask(affinity), before = ProcessAffinity();
    if (!mask) {
      REXLOG_ERROR("Any360: any360_cpu_affinity \"{}\" is not a processor mask or list", affinity);
    } else if (KeepProcessAffinity(mask)) {
      REXLOG_INFO("Any360: the title runs on processors mask 0x{:X} (0x{:X} before)", ProcessAffinity(), before);
    } else {
      REXLOG_ERROR("Any360: the system refused processors mask 0x{:X}", mask);
    }
  }
  const std::vector<std::string> groups = SystemGroupsNamed(REXCVAR_GET(any360_sys_groups), g_default_groups);
  if (groups.empty()) return false;
  SystemOptions options;
  options.trace_calls = REXCVAR_GET(any360_sys_trace);
  options.player_slots = REXCVAR_GET(any360_input_players);
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
    if (group == SysGroup::kThreads) {
      any360::sys::Threads().SetDefaultStackSize(uint32_t(std::max(0, REXCVAR_GET(any360_sys_default_stack_kb))) * 1024);
      any360::sys::Threads().SetIdleBackoff(REXCVAR_GET(any360_sys_idle_backoff));
      any360::sys::Threads().SetSpinPacing(REXCVAR_GET(any360_sys_spin_pacing),
                                           [](const std::string& line) { REXLOG_INFO("Any360 system: {}", line); });
      any360::sys::Threads().SetStackLog([](const std::string& line) { REXLOG_WARN("Any360 system: {}", line); },
                                         std::chrono::seconds(2), REXCVAR_GET(any360_sys_log_threads));
      REXLOG_INFO("Any360 system: threads created with no stack size get {} KB",
                  any360::sys::Threads().default_stack_size() / 1024);
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
      if (const std::string text = REXCVAR_GET(any360_input_script); !text.empty()) {
        std::string error;
        any360::sys::InputScript script = any360::sys::InputScript::Parse(text, &error);
        if (!error.empty()) REXLOG_WARN("Any360 system: input script entries not understood: {}", error);
        REXLOG_INFO("Any360 system: input script of {} presses on the first controller", script.presses().size());
        any360::sys::XamState().SetInputScript(std::move(script));
      }
      const auto from = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(std::max(0.0, REXCVAR_GET(any360_input_record_from))));
      if (const std::string path = REXCVAR_GET(any360_input_replay); !path.empty()) {
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        any360::sys::InputRecording recording;
        std::string error;
        if (!any360::sys::InputRecording::Parse(text, recording, &error)) {
          REXLOG_WARN("Any360 system: input replay {} not read: {}", path, error);
        } else {
          REXLOG_INFO("Any360 system: input replay of {} changes ({:.1f} s) on the first controller from {:.1f} s",
                      recording.events().size(), std::chrono::duration<double>(recording.length()).count(),
                      REXCVAR_GET(any360_input_record_from));
          any360::sys::XamState().SetInputReplay(std::move(recording), from);
        }
      }
      if (const std::string path = REXCVAR_GET(any360_input_record); !path.empty()) {
        const auto length = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(std::max(0.0, REXCVAR_GET(any360_input_record_seconds))));
        if (any360::sys::XamState().SetInputRecorder(path, from, length)) {
          REXLOG_INFO("Any360 system: recording the first controller to {} from {:.1f} s", path,
                      REXCVAR_GET(any360_input_record_from));
        } else {
          REXLOG_WARN("Any360 system: input recording {} cannot be written", path);
        }
      }
      // The bindings: the launcher's file if any, over Any360's own keyboard and mouse layout (kDefaultKeyboard), so
      // the keyboard plays and shows its glyphs with Any360's keys where the file names none.
      any360::sys::InputBindings file;
      const std::string path = REXCVAR_GET(any360_input_bindings);
      if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && text.empty()) {
          REXLOG_WARN("Any360 system: no input bindings file at {}", path);
        } else {
          std::vector<std::string> problems;
          file = any360::sys::InputBindings::Parse(text, &problems);
          for (const std::string& p : problems) REXLOG_WARN("Any360 system: input bindings: {}", p);
          const any360::sys::BindingProfile* pad = file.Find("gamepad");
          REXLOG_INFO("Any360 system: input bindings from {}: {} profiles, the controller's {} targets", path,
                      file.profiles().size(), pad ? pad->bindings.size() : 0);
        }
      }
      {
        const any360::sys::InputBindings bindings = file.WithDefaultKeyboard();
        any360::sys::XamState().SetInputBindings(bindings);
        SetPlayerBindings(bindings);
        // The keyboard and mouse player's layout, through the runtime's keyboard driver until Any360 reads the keys
        // itself (keyboard_binds.h): every control named, so the driver follows Any360's layout alone.
        std::vector<std::string> key_problems;
        const std::vector<RuntimeSetting> keys = RuntimeKeybinds(bindings, &key_problems);
        for (const std::string& p : key_problems) REXLOG_WARN("Any360 system: keyboard bindings: {}", p);
        size_t applied = 0;
        for (const RuntimeSetting& s : keys) {
          if (rex::cvar::SetFlagByName(s.name, s.value)) {
            ++applied;
          } else {
            REXLOG_WARN("Any360 system: keyboard bindings: the runtime has no setting {}", s.name);
          }
        }
        REXLOG_INFO("Any360 system: keyboard and mouse layout: {} settings{}", applied,
                    file.Find("keyboard_mouse") ? " (the file's over Any360's)" : " (Any360's)");
      }
      if (const int player = REXCVAR_GET(any360_keyboard_player); player >= 0) {
        const bool on = player >= 1 && player <= 4;
        if (!rex::cvar::SetFlagByName("mnk_mode", on ? "true" : "false")) {
          REXLOG_WARN("Any360 system: the runtime has no keyboard driver setting (mnk_mode)");
        }
        if (on) SetKeyboardPlayer(uint32_t(player - 1));
        REXLOG_INFO("Any360 system: keyboard and mouse {}", on ? "on player " + std::to_string(player) : std::string("off"));
      }
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

namespace any360::host::rexglue {
bool FpsOverlay() { return REXCVAR_GET(any360_fps_overlay); }
int HitchTraceMs() { return REXCVAR_GET(any360_hitch_trace_ms); }
std::string HitchTraceFolder() { return REXCVAR_GET(any360_hitch_trace_folder); }
int ProfileSeconds() { return REXCVAR_GET(any360_profile_seconds); }
int ProfileAfterSeconds() { return REXCVAR_GET(any360_profile_after); }
}  // namespace any360::host::rexglue
