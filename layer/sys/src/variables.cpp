#include "any360/sys/variables.h"

#include <chrono>
#include <cstring>

#include "any360/sys/objects.h"
#include "any360/sys/xam.h"

namespace any360::sys {

namespace {

const auto kBoot = std::chrono::steady_clock::now();

uint32_t FourCC(const char* tag) { return uint32_t(tag[0]) << 24 | uint32_t(tag[1]) << 16 | uint32_t(tag[2]) << 8 | uint32_t(tag[3]); }

// The OBJECT_TYPE descriptors (0x1C bytes: constructor, destructor, four unknown words, the pool tag).
struct ObjectTypeVariable {
  const char* name;
  const char* pool_tag;
};
constexpr ObjectTypeVariable kObjectTypes[] = {
    {"ExThreadObjectType", "Thrd"},    {"ExEventObjectType", "Evnt"},   {"ExMutantObjectType", "Muta"},
    {"ExSemaphoreObjectType", "Sema"}, {"ExTimerObjectType", "Timr"},   {"IoCompletionObjectType", "IoCp"},
    {"IoDeviceObjectType", "IoDv"},    {"IoFileObjectType", "IoFl"},    {"ObDirectoryObjectType", "ObDr"},
    {"ObSymbolicLinkObjectType", "ObSl"},
};

}  // namespace

KernelVariables& Variables() {
  static KernelVariables variables;
  return variables;
}

KernelVariables::~KernelVariables() { Reset(); }

void KernelVariables::Reset() {
  ticking_ = false;
  if (clock_.joinable()) clock_.join();
  std::lock_guard lock(mutex_);
  addresses_.clear();
  time_stamp_bundle_ = 0;
}

uint32_t KernelVariables::Address(Guest g, const std::string& name) {
  std::lock_guard lock(mutex_);
  if (const auto it = addresses_.find(name); it != addresses_.end()) return it->second;
  const uint32_t address = Create(g, name);
  if (address) addresses_[name] = address;
  return address;
}

void KernelVariables::Tick(Guest g) {
  const uint32_t bundle = time_stamp_bundle_;
  if (!bundle) return;
  const auto since_boot = std::chrono::steady_clock::now() - kBoot;
  g.Store64(bundle, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(since_boot).count() / 100));
  g.Store64(bundle + 8, SystemTime());
  g.Store32(bundle + 16, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(since_boot).count()));
}

uint32_t KernelVariables::Create(Guest g, const std::string& name) {
  auto block = [&](uint32_t size) {
    const uint32_t at = Objects().AllocateObject(size);
    if (at) std::memset(g.host(at), 0, size);
    return at;
  };
  for (const ObjectTypeVariable& type : kObjectTypes) {
    if (name != type.name) continue;
    const uint32_t at = block(0x1C);
    if (at) g.Store32(at + 0x18, FourCC(type.pool_tag));
    return at;
  }
  const TitleSettings& title = XamState().title();
  if (name == "XboxHardwareInfo") {
    // Flags (0x20: the hard drive is present, set once storage is up), then the number of hardware threads.
    const uint32_t at = block(16);
    if (at) {
      g.Store32(at, 0x20);
      g.Store8(at + 4, 6);
    }
    return at;
  }
  if (name == "XboxKrnlVersion") {
    // Major, minor, build, then the QFE byte pair: 2.0xFFFF.0xFFFF.0x80, newer than any title asks for.
    const uint32_t at = block(8);
    if (at) {
      g.Store16(at, 2);
      g.Store16(at + 2, 0xFFFF);
      g.Store16(at + 4, 0xFFFF);
      g.Store8(at + 6, 0x80);
    }
    return at;
  }
  if (name == "ExConsoleGameRegion") {
    const uint32_t at = block(4);
    if (at) g.Store32(at, 0xFFFFFFFF);  // every region
    return at;
  }
  if (name == "KeDebugMonitorData" || name == "KeCertMonitorData") return block(4);  // no debugger, no cert monitor
  if (name == "XexExecutableModuleHandle") {
    const uint32_t at = block(4);
    if (at) g.Store32(at, title.module_handle);
    return at;
  }
  if (name == "ExLoadedImageName") {
    // The executable's full path (256 bytes).
    const uint32_t at = block(256);
    const std::string path = "\\Device\\Cdrom0\\" + title.module_name;
    if (at) std::memcpy(g.host(at), path.data(), std::min<size_t>(path.size(), 255));
    return at;
  }
  if (name == "ExLoadedCommandLine") {
    // The quoted executable name, in a buffer rounded to 1 KB.
    const std::string line = "\"" + title.module_name + "\"";
    const uint32_t at = block(uint32_t((line.size() + 1 + 1023) & ~size_t(1023)));
    if (at) std::memcpy(g.host(at), line.data(), line.size());
    return at;
  }
  if (name == "KeTimeStampBundle") {
    // Interrupt time and system time (100 ns), the tick count (ms), kept current by a clock thread every millisecond.
    const uint32_t at = block(24);
    if (!at) return 0;
    time_stamp_bundle_ = at;
    Tick(g);
    ticking_ = true;
    clock_ = std::thread([this, g] {
      while (ticking_) {
        Tick(g);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
    return at;
  }
  return 0;
}

}  // namespace any360::sys
