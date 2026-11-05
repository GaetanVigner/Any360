// any360::host::rexglue, the system calls: see the header.
#include "any360/host/system.h"

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xevent.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "any360/sys/exports.h"
#include "any360/sys/files.h"
#include "any360/sys/guest.h"
#include "any360/sys/memory.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"
#include "any360/sys/xam.h"

namespace any360::host::rexglue {

namespace {

constexpr const char* kGroupNames[] = {"stateless", "files", "objects", "threads", "memory", "xam", "input", "audio"};
static_assert(std::size(kGroupNames) == size_t(SysGroup::kCount));

std::array<std::atomic<bool>, size_t(SysGroup::kCount)> g_enabled{};
std::atomic<bool> g_started{false};
uint8_t* g_base = nullptr;
std::function<void(const std::string&)> g_log;
std::atomic<bool> g_trace{false};
std::atomic<bool> g_trace_every{false};
std::atomic<uint32_t> g_trace_groups{~0u};  // the groups every-call tracing reports (a bit per SysGroup)
// Calls kept on the runtime though their group is switched (set once by StartSystem, before the title's calls).
std::unordered_set<std::string> g_runtime_calls;
std::unordered_set<std::string> g_trace_names;  // when not empty, the only calls every-call tracing reports
std::atomic<bool> g_any_runtime_calls{false};
std::mutex g_traced_mutex;
std::unordered_set<const char*> g_traced;  // the names' literals, one each

// The first call of `name` on any360::sys, reported once.
void Trace(SysGroup group, const char* name) {
  {
    std::lock_guard lock(g_traced_mutex);
    if (!g_traced.insert(name).second) return;
  }
  if (g_log) g_log(std::string("first call on any360::sys: ") + name + " (" + kGroupNames[size_t(group)] + ")");
}

}  // namespace

const char* SysGroupName(SysGroup group) { return group < SysGroup::kCount ? kGroupNames[size_t(group)] : "?"; }

bool ParseSysGroup(const std::string& name, SysGroup& group) {
  for (size_t i = 0; i < std::size(kGroupNames); ++i) {
    if (name == kGroupNames[i]) {
      group = SysGroup(i);
      return true;
    }
  }
  return false;
}

void EnableSystemGroup(SysGroup group, bool enabled) {
  if (group < SysGroup::kCount) g_enabled[size_t(group)] = enabled;
}

bool SystemGroupEnabled(SysGroup group) { return group < SysGroup::kCount && g_enabled[size_t(group)]; }

bool Enabled(SysGroup group) { return g_enabled[size_t(group)].load(std::memory_order_relaxed); }

bool TraceEvery(SysGroup group, const char* name) {
  return g_trace_every.load(std::memory_order_relaxed) &&
         (g_trace_groups.load(std::memory_order_relaxed) >> uint32_t(group) & 1) &&
         (g_trace_names.empty() || g_trace_names.count(name));
}

void RunAny360(SysGroup group, const char* name, GuestFunction any360, PPCContext& ctx, uint8_t* base) {
  if (g_trace.load(std::memory_order_relaxed)) Trace(group, name);
  if (!TraceEvery(group, name)) {
    any360(ctx, base);
    return;
  }
  char line[200];
  const int n = std::snprintf(line, sizeof(line), "%s(%08X %08X %08X %08X %08X %08X %08X %08X) lr %08X", name, ctx.r3.u32,
                              ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                              uint32_t(ctx.lr));
  if (g_log && std::strstr(name, "Wait")) g_log(std::string("waiting: ") + line);  // a wait that never returns shows
  any360(ctx, base);
  std::snprintf(line + n, sizeof(line) - n, " = %08X", ctx.r3.u32);
  if (g_log) g_log(line);
}

// The runtime's export, reported like RunAny360's calls (marked "runtime") with every-call tracing when an argument is
// one of any360::sys's handles: the runtime cannot use those.
void RunReference(const char* name, GuestFunction reference, PPCContext& ctx, uint8_t* base) {
  if (!g_trace_every.load(std::memory_order_relaxed)) {
    reference(ctx, base);
    return;
  }
  // Only the calls handed one of any360::sys's handles (the runtime knows nothing of them).
  const uint32_t registers[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  bool ours = false;
  for (uint32_t value : registers) ours = ours || any360::sys::Objects().OwnsHandle(value);
  if (!ours) {
    reference(ctx, base);
    return;
  }
  char line[200];
  const int n = std::snprintf(line, sizeof(line), "runtime %s(%08X %08X %08X %08X %08X %08X %08X %08X) lr %08X", name,
                              ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
                              ctx.r10.u32, uint32_t(ctx.lr));
  reference(ctx, base);
  std::snprintf(line + n, sizeof(line) - n, " = %08X", ctx.r3.u32);
  if (g_log) g_log(line);
}

void CallSystemExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PPCContext& ctx,
                      uint8_t* base) {
  const bool kept = reference && g_any_runtime_calls.load(std::memory_order_relaxed) && g_runtime_calls.count(name);
  if (!reference || (!kept && g_started.load(std::memory_order_relaxed) && g_enabled[size_t(group)].load(std::memory_order_relaxed))) {
    RunAny360(group, name, any360, ctx, base);
  } else {
    RunReference(name, reference, ctx, base);
  }
}

void CallHandleExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, uint32_t handle,
                      PPCContext& ctx, uint8_t* base) {
  bool ours;
  if (!reference) ours = true;
  else if (!g_started.load(std::memory_order_relaxed)) ours = false;
  else if (handle >= 0xFFFFFFF0) ours = Enabled(group);  // the current thread or process
  else ours = any360::sys::Objects().OwnsHandle(handle);
  if (ours) RunAny360(group, name, any360, ctx, base);
  else RunReference(name, reference, ctx, base);
}

void CallPathExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PathKind kind,
                    uint32_t path, PPCContext& ctx, uint8_t* base) {
  bool ours = !reference;
  if (!ours && g_started.load(std::memory_order_relaxed) && Enabled(group) && path) {
    ours = kind == PathKind::kAttributes
               ? any360::sys::Files().KnowsAttributes(any360::sys::Guest{base}, path)
               : any360::sys::Files().Knows(reinterpret_cast<const char*>(any360::sys::Guest{base}.host(path)));
  }
  if (ours && TraceEvery(group, name) && g_log && path) {
    std::string text;
    if (kind == PathKind::kAttributes) {
      // OBJECT_ATTRIBUTES: root, then the name (an ANSI_STRING: length, maximum, buffer).
      const any360::sys::Guest g{base};
      const uint32_t string = g.Load32(path + 4);
      if (string) text.assign(reinterpret_cast<const char*>(g.host(g.Load32(string + 4))), g.Load16(string));
    } else {
      text = reinterpret_cast<const char*>(any360::sys::Guest{base}.host(path));
    }
    g_log(std::string(name) + " path " + text);
  }
  if (ours) RunAny360(group, name, any360, ctx, base);
  else RunReference(name, reference, ctx, base);
}

void CallBothExport(SysGroup group, const char* name, GuestFunction any360, GuestFunction reference, PPCContext& ctx,
                    uint8_t* base) {
  if (reference && !(g_started.load(std::memory_order_relaxed) && Enabled(group))) {
    RunReference(name, reference, ctx, base);
    return;
  }
  PPCContext copy = ctx;
  RunAny360(group, name, any360, ctx, base);
  if (reference) RunReference(name, reference, copy, base);
}

// The heaps any360::sys's memory manager keeps (MemoryManager::Reset's layout).
constexpr std::pair<uint32_t, uint32_t> kSharedHeaps[] = {{0x00000000, 0x40000000}, {0x40000000, 0x3F000000},
                                                         {0xA0000000, 0x20000000}, {0xC0000000, 0x20000000},
                                                         {0xE0000000, 0x1FD00000}, {0x8C000000, 0x04000000}};

// The runtime's regions in [low, low + size) marked as its own in any360::sys's memory manager.
static void ImportRuntimeRange(uint32_t low, uint32_t size) {
  auto* memory = REX_KERNEL_STATE()->memory();
  const uint64_t end = uint64_t(low) + size;
  for (uint64_t address = low; address < end;) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(uint32_t(address));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
      address += 0x1000;
      continue;
    }
    if (info.state) {
      any360::sys::Memory().MarkForeign(uint32_t(address), uint32_t(std::min<uint64_t>(info.region_size, end - address)));
    }
    address += info.region_size;
  }
}

static bool ClaimInRuntime(uint32_t address, uint32_t size, uint32_t, void*) {
  rex::memory::BaseHeap* heap = REX_KERNEL_STATE()->memory()->LookupHeap(address);
  if (!heap) return true;  // a range the runtime does not manage
  // Committed read-write there: the runtime keeps the host pages accessible for the guest code.
  if (heap->AllocFixed(address, size, heap->page_size(),
                       rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                       rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    return true;
  }
  ImportRuntimeRange(address, size);  // what the runtime took since, so the search moves past it
  return false;
}

static void ReleaseInRuntime(uint32_t address, void*) {
  if (rex::memory::BaseHeap* heap = REX_KERNEL_STATE()->memory()->LookupHeap(address)) heap->Release(address);
}

static void RunInRuntimeThread(void (*body)(void*), void* arg, void*) {
  auto* kernel = REX_KERNEL_STATE();
  auto thread = rex::system::make_object<rex::system::XHostThread>(kernel, 16 * 1024, 0, [body, arg] {
    body(arg);
    return 0;
  });
  if (!kernel || thread->Create() != 0) {
    body(arg);  // no runtime thread to be: run as before
    return;
  }
  thread->Wait(0, 0, 0, nullptr);
}

uint64_t DispatchGuest(uint32_t address, uint32_t stack_top, uint32_t pcr, const uint64_t* args, uint32_t count, void*) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->emulator()) return 0;
  ::PPCFunc* function = kernel->emulator()->function_dispatcher()->GetFunction(address);
  if (!function) return 0;
  // A context of its own: guest code a host-driven callback, an APC or a DPC runs leaves the caller's registers alone.
  // The floating-point state starts as the runtime's threads do.
  alignas(64) PPCContext ctx{};
  ctx.fpscr.InitHost();
  ctx.r1.u64 = stack_top;
  ctx.r13.u64 = pcr;
  PPCRegister* const registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (uint32_t i = 0; i < count && i < any360::sys::kMaxDispatchArguments; ++i) registers[i]->u64 = args[i];
  function(ctx, kernel->memory()->virtual_membase());
  return ctx.r3.u64;
}

bool VariableGroup(const char* name, SysGroup& group) {
  static const struct {
    const char* name;
    SysGroup group;
  } kGroups[] = {
      {"XboxHardwareInfo", SysGroup::kStateless},       {"XboxKrnlVersion", SysGroup::kStateless},
      {"ExConsoleGameRegion", SysGroup::kStateless},    {"KeDebugMonitorData", SysGroup::kStateless},
      {"KeCertMonitorData", SysGroup::kStateless},      {"KeTimeStampBundle", SysGroup::kThreads},
      // Not the object types (ExEventObjectType...): a title only hands them back to ObReferenceObjectByHandle and
      // its kin, which any360::sys does not check and the runtime does against its own, for the handles it keeps.
      {"XexExecutableModuleHandle", SysGroup::kXam},    {"ExLoadedImageName", SysGroup::kXam},
      {"ExLoadedCommandLine", SysGroup::kXam},
  };
  for (const auto& entry : kGroups) {
    if (!std::strcmp(entry.name, name)) {
      group = entry.group;
      return true;
    }
  }
  return false;
}

uint32_t BindVariables() {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !g_base) return 0;
  auto module = kernel->GetExecutableModule();
  if (!module || !module->xex_module()) return 0;
  auto* resolver = kernel->emulator()->function_dispatcher()->export_resolver();
  const any360::sys::Guest g{g_base};
  uint32_t bound = 0;
  for (const auto& library : *module->xex_module()->import_libraries()) {
    for (const auto& import : library.imports) {
      if (!import.value_address) continue;
      const rex::runtime::Export* entry = resolver->GetExportByOrdinal(library.name, uint16_t(import.ordinal));
      if (!entry || entry->type != rex::runtime::Export::Type::kVariable) continue;
      SysGroup group;
      if (!VariableGroup(entry->name, group) || !SystemGroupEnabled(group)) continue;
      if (const uint32_t address = any360::sys::VariableAddress(g_base, entry->name)) {
        // The import table sits in the image, read-only once loaded: writable for the store, then as it was.
        rex::memory::BaseHeap* heap = kernel->memory()->LookupHeap(import.value_address);
        uint32_t protect = 0;
        if (heap) heap->Protect(import.value_address, 4, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, &protect);
        g.Store32(import.value_address, address);
        if (heap) heap->Protect(import.value_address, 4, protect);
        ++bound;
      }
    }
  }
  return bound;
}

bool StartSystem(const SystemOptions& options, std::string& error) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel || !kernel->memory()) {
    error = "the runtime is not set up";
    return false;
  }
  g_base = kernel->memory()->virtual_membase();
  g_log = options.log;
  g_trace = options.trace_calls;
  g_trace_every = options.trace_every_call;
  uint32_t traced = 0;
  for (SysGroup group : options.trace_groups) traced |= 1u << uint32_t(group);
  g_trace_groups = options.trace_groups.empty() ? ~0u : traced;
  g_runtime_calls = std::unordered_set<std::string>(options.runtime_calls.begin(), options.runtime_calls.end());
  g_any_runtime_calls = !g_runtime_calls.empty();
  g_trace_names = std::unordered_set<std::string>(options.trace_names.begin(), options.trace_names.end());
  const uint32_t heap = kernel->memory()->SystemHeapAlloc(options.kernel_heap_size, 0x10000);
  if (!heap) {
    error = "no room in the runtime's system heap for the kernel heap";
    return false;
  }
  std::memset(g_base + heap, 0, options.kernel_heap_size);
  // The runtime's physical view from 0xE0000000 (Memory::MapViews, PhysicalHostOffset): the title's code and any360::sys
  // must find the same bytes there.
  const auto* physical = kernel->memory()->LookupHeap(0xE0000000);
  any360::sys::SetPhysicalHostOffset(physical ? physical->host_address_offset() : 0);
  any360::sys::Objects().SetGuest(any360::sys::Guest{g_base});
  any360::sys::Objects().SetKernelHeap(heap, options.kernel_heap_size);
  any360::sys::Objects().SetHandleBase(options.handle_base);
  // The runtime's handles, for a group switched here that takes an object the runtime made (an event a file read sets).
  any360::sys::Objects().SetForeignHandles(
      {[](uint32_t handle) { return bool(REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle)); },
       [](uint32_t handle) -> uint32_t {
         auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle);
         if (!event) return 0xC0000008;  // STATUS_INVALID_HANDLE
         event->Set(0, false);
         return 0;
       }});
  any360::sys::Objects().Reset();
  any360::sys::Files().Reset();
  for (const SystemMount& mount : options.mounts) {
    any360::sys::Files().Mount(mount.device, mount.host, mount.read_only);
    for (const std::string& link : mount.links) any360::sys::Files().Link(link, mount.device);
  }
  any360::sys::Threads().SetDispatcher(&DispatchGuest, nullptr);
  // The runtime answers calls (its XAM, audio, input, memory while they are its) only on its own threads: any360::sys's
  // threads run inside one each (an XHostThread, whose KTHREAD and PCR the guest code never sees).
  any360::sys::Threads().SetThreadRunner(&RunInRuntimeThread, nullptr);
  // One guest address space, two allocators: any360::sys's memory manager starts from the runtime's page map and
  // claims each range it hands out in the runtime's heaps, which go on serving the runtime's own allocations.
  any360::sys::Memory().Reset();
  any360::sys::Memory().SetSharedOwner({&ClaimInRuntime, &ReleaseInRuntime, nullptr});
  for (const auto& [low, size] : kSharedHeaps) ImportRuntimeRange(low, size);
  // The title as the runtime loaded it: XamGetCurrentTitleId, RtlImageXexHeaderField's callers and the variables
  // (XexExecutableModuleHandle, ExLoadedImageName...) answer from these.
  g_started = true;
  if (auto module = kernel->GetExecutableModule()) {
    any360::sys::TitleSettings& title = any360::sys::XamState().title();
    title.module_handle = module->hmodule_ptr();
    title.xex_header = module->guest_xex_header();
    title.title_id = module->title_id();
    const std::string& path = module->path();
    const size_t slash = path.find_last_of("\\/:");
    title.module_name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (auto* profile = kernel->user_profile()) {
      title.xuid = profile->xuid();
      title.gamertag = profile->name();
    }
    if (!options.user_data_root.empty()) {
      char xuid[17], title_id[9];
      std::snprintf(xuid, sizeof xuid, "%016llX", static_cast<unsigned long long>(title.xuid));
      std::snprintf(title_id, sizeof title_id, "%08X", title.title_id);
      const std::filesystem::path root(options.user_data_root);
      title.content_root = root / xuid / title_id;
      title.profile_root = root / title_id / "profile" / title.gamertag;
      if (g_log) {
        g_log("XAM content at " + title.content_root.string() + ", profile settings at " + title.profile_root.string() +
              ", profile " + title.gamertag);
      }
    }
  }
  return true;
}

}  // namespace any360::host::rexglue
