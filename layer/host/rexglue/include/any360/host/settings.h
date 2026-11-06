// The system binding driven by settings, the same for every ReXGlue title: the any360_sys_* settings (the groups to
// switch, the traces, calls kept on the runtime) read, any360::sys started with the mounts a console title expects,
// the audio hardware started when the audio group is asked for. A title's whole system binding is then one source
// file holding `#include "any360/host/system_exports.inc"` (the overrides must be defined in the executable) and one
// call to StartSystemFromSettings once the image is loaded (OnPostSetup).
//
// Settings: any360_sys_groups (comma-separated: stateless, files, objects, threads, memory, xam, input, audio; empty:
// the binding stays off), any360_sys_trace, any360_sys_trace_all, any360_sys_trace_groups, any360_sys_trace_names,
// any360_sys_runtime_calls (see SystemOptions).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "system.h"

namespace any360::host::rexglue {

// Starts the system binding when any360_sys_groups names groups: the title's files mounted as the runtime mounts them
// (\Device\Harddisk0\Partition1 as game: and d:, read-only unless allow_game_relative_writes), saves and profile
// settings where the runtime keeps them (user_data_root), plus `title_mounts` (devices the title registers on the
// runtime's file system itself). False when the binding is off or could not start (logged).
bool StartSystemFromSettings(const std::filesystem::path& game_data_root, const std::filesystem::path& user_data_root,
                             const std::vector<SystemMount>& title_mounts = {});

// Stops the system binding's own threads (the audio pump, the XMA decoder) before the runtime shuts down
// (OnShutdown): a title that exits through the runtime's shutdown needs it, one that ends the process does not.
void StopSystemFromSettings();

// A host folder over a guest folder on any360::sys's file system too (read-only), for the redirections a title sets
// up on the runtime's file system (a mod or language folder). Nothing while the binding is off.
void MountFolderIfStarted(const std::string& guest_folder, const std::filesystem::path& host);

}  // namespace any360::host::rexglue
