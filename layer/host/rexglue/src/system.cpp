// any360::host::rexglue, the system calls: see the header.
#include "any360/host/system.h"

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

#include <array>
#include <atomic>
#include <cstring>

#include "any360/sys/exports.h"
#include "any360/sys/guest.h"
#include "any360/sys/objects.h"
#include "any360/sys/scheduler.h"

namespace any360::host::rexglue {

namespace {

constexpr const char* kGroupNames[] = {"stateless", "files", "objects", "threads", "memory", "xam", "input", "audio"};
static_assert(std::size(kGroupNames) == size_t(SysGroup::kCount));

std::array<std::atomic<bool>, size_t(SysGroup::kCount)> g_enabled{};
uint8_t* g_base = nullptr;

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

void CallSystemExport(SysGroup group, GuestFunction any360, GuestFunction reference, PPCContext& ctx, uint8_t* base) {
  if (!reference || g_enabled[size_t(group)].load(std::memory_order_relaxed)) {
    any360(ctx, base);
  } else {
    reference(ctx, base);
  }
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
      if (const uint32_t address = any360::sys::VariableAddress(g_base, entry->name)) {
        g.Store32(import.value_address, address);
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
  const uint32_t heap = kernel->memory()->SystemHeapAlloc(options.kernel_heap_size, 0x10000);
  if (!heap) {
    error = "no room in the runtime's system heap for the kernel heap";
    return false;
  }
  std::memset(g_base + heap, 0, options.kernel_heap_size);
  any360::sys::Objects().SetGuest(any360::sys::Guest{g_base});
  any360::sys::Objects().SetKernelHeap(heap, options.kernel_heap_size);
  any360::sys::Objects().Reset();
  any360::sys::Threads().SetDispatcher(&DispatchGuest, nullptr);
  if (options.variables) BindVariables();
  return true;
}

}  // namespace any360::host::rexglue
