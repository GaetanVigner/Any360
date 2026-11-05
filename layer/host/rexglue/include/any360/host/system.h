// any360::host::rexglue, the system calls: a ReXGlue title runs its kernel on any360::sys a group at a time
// (docs/SYS_BINDING.md), every call of a group switched together. Usage in the title:
//
//   #include "any360/host/system.h"
//   #define ANY360_REXGLUE_SKIP_XamInputGetState  // a call the title hooks itself: left out of the overrides
//   #include "any360/host/system_exports.inc"     // once, at namespace scope: an override per call
//
//   // Once the runtime is set up and the title loaded, before its first instruction:
//   std::string error;
//   if (any360::host::rexglue::StartSystem({}, error)) {
//     any360::host::rexglue::EnableSystemGroup(any360::host::rexglue::SysGroup::kStateless, true);
//     any360::host::rexglue::BindVariables();  // the variables of the groups switched
//   }
//
// A group left off (the default) calls the runtime's own exports, so one build runs either way. The video calls are
// the graphics binding's (rexglue.h).
#pragma once

#include <rex/ppc.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "any360/host/rexglue.h"

namespace any360::host::rexglue {

// The groups, in the order the plan switches them.
enum class SysGroup : uint32_t { kStateless, kFiles, kObjects, kThreads, kMemory, kXam, kInput, kAudio, kCount };

// "stateless", "files", "objects", "threads", "memory", "xam", "input", "audio": the names a title's settings use.
const char* SysGroupName(SysGroup group);
bool ParseSysGroup(const std::string& name, SysGroup& group);

// A device of the title's file system and the links to it, mirrored on any360::sys's.
struct SystemMount {
  std::string device;  // \Device\Harddisk0\Partition1
  std::string host;    // the host directory
  bool read_only = true;
  std::vector<std::string> links;  // game:, d:
};

struct SystemOptions {
  // Bytes of the runtime's system heap given to any360::sys's object manager as its kernel heap (the objects,
  // threads' KTHREADs, PCRs and stacks, the variables it creates).
  uint32_t kernel_heap_size = 64 << 20;
  // Where the binding reports, with `trace_calls`: the first time each call goes to any360::sys (the calls a title
  // really makes of the groups switched). None: nothing reported.
  std::function<void(const std::string&)> log;
  bool trace_calls = false;
  // Every call that goes to any360::sys, reported with its first eight arguments and its result (r3), a path call with its path, for finding the
  // call whose answer a title did not expect. Slow: for a debugging run.
  bool trace_every_call = false;
  // Where any360::sys's handles start: apart from the runtime's (0xF8000000 up), so a handle names its owner.
  uint32_t handle_base = 0xF8100000;
  // The devices the runtime mounted (the title's files, its cache partition...), mounted the same on any360::sys.
  std::vector<SystemMount> mounts;
};

// Prepares any360::sys inside the running runtime: the guest memory, the kernel heap, the dispatcher its scheduler
// runs guest code through, the title's settings (its module, XEX header, title id). False with `error` when the runtime is not set up or the heap cannot
// be had: nothing is switched then.
bool StartSystem(const SystemOptions& options, std::string& error);
// Switches a group to Any360 (or back). Meant once, before the title's first call of the group: calls of a group
// share state, which does not move from one side to the other.
void EnableSystemGroup(SysGroup group, bool enabled);
bool SystemGroupEnabled(SysGroup group);

// One call: Any360's when its group is on or the runtime has no such export, the runtime's otherwise. Until StartSystem
// every call is the runtime's.
void CallSystemExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PPCContext& ctx,
                      uint8_t* base);
// A call taking a handle: the side that made the handle (pseudo handles: as the group says).
void CallHandleExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, uint32_t handle,
                      PPCContext& ctx, uint8_t* base);
// A call naming a path: Any360's when its group is on and its file system knows the path's device.
enum class PathKind { kAttributes, kString };
void CallPathExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PathKind kind,
                    uint32_t path, PPCContext& ctx, uint8_t* base);
// A call both sides must see: with its group on, Any360's (its answer) and then the runtime's.
void CallBothExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PPCContext& ctx,
                    uint8_t* base);

// any360::sys's Dispatcher on the runtime: runs the guest function at `address` (the runtime's function table) on the
// calling host thread, with r1 = `stack_top`, r13 = `pcr` and r3 onwards = `args`; returns r3, 0 for an address with
// no function.
uint64_t DispatchGuest(uint32_t address, uint32_t stack_top, uint32_t pcr, const uint64_t* args, uint32_t count, void* user);

// Points the executable's variable imports any360::sys provides at its variables, for the groups switched on: each
// variable belongs to the group whose calls read it (the object types to the objects, whose Ob* calls compare
// against them; the time stamp bundle to the threads; the loaded image's names to XAM; the console's constants to the
// stateless calls). Call after EnableSystemGroup; returns the number pointed.
uint32_t BindVariables();
// The group a variable belongs to; false for a name the binding does not provide.
bool VariableGroup(const char* name, SysGroup& group);

}  // namespace any360::host::rexglue

// One override: the title's generated code calls `__imp__<name>`, which a definition in the title takes from the
// runtime's import library. The runtime's own export is looked up once.
#define ANY360_REXGLUE_BIND_SYSTEM(name, group)                                                                    \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                                    \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                                        \
    static const ::any360::host::rexglue::GuestFunction reference =                                                 \
        ::any360::host::rexglue::ReferenceExport("__imp__" #name);                                                  \
    ::any360::host::rexglue::CallSystemExport(::any360::host::rexglue::SysGroup::group, #name, &any360_##name, reference, \
                                              ctx, base);                                                                \
  }

#define ANY360_REXGLUE_SYSTEM_REFERENCE(name)                                \
  static const ::any360::host::rexglue::GuestFunction reference =          \
      ::any360::host::rexglue::ReferenceExport("__imp__" #name)

#define ANY360_REXGLUE_BIND_HANDLE(name, group, reg)                                                                    \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                                         \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                                             \
    ANY360_REXGLUE_SYSTEM_REFERENCE(name);                                                                               \
    ::any360::host::rexglue::CallHandleExport(::any360::host::rexglue::SysGroup::group, #name, &any360_##name, reference, \
                                              ctx.reg.u32, ctx, base);                                                   \
  }

#define ANY360_REXGLUE_BIND_HANDLE_AT(name, group, reg)                                                                 \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                                         \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                                             \
    ANY360_REXGLUE_SYSTEM_REFERENCE(name);                                                                               \
    uint32_t handle = 0;                                                                                                 \
    if (ctx.reg.u32) handle = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctx.reg.u32));                 \
    ::any360::host::rexglue::CallHandleExport(::any360::host::rexglue::SysGroup::group, #name, &any360_##name, reference, \
                                              handle, ctx, base);                                                        \
  }

#define ANY360_REXGLUE_BIND_PATH(name, group, reg, kind)                                                                \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                                         \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                                             \
    ANY360_REXGLUE_SYSTEM_REFERENCE(name);                                                                               \
    ::any360::host::rexglue::CallPathExport(::any360::host::rexglue::SysGroup::group, #name, &any360_##name, reference,   \
                                            ::any360::host::rexglue::PathKind::kind, ctx.reg.u32, ctx, base);            \
  }

#define ANY360_REXGLUE_BIND_BOTH(name, group)                                                                           \
  extern "C" void any360_##name(PPCContext& ctx, uint8_t* base);                                                         \
  extern "C" void __imp__##name(PPCContext& __restrict ctx, uint8_t* base) {                                             \
    ANY360_REXGLUE_SYSTEM_REFERENCE(name);                                                                               \
    ::any360::host::rexglue::CallBothExport(::any360::host::rexglue::SysGroup::group, #name, &any360_##name, reference,   \
                                            ctx, base);                                                                  \
  }
