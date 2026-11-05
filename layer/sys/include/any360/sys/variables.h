// The variables xboxkrnl.exe exports: guest data a title imports by address rather than calls (XboxHardwareInfo,
// KeTimeStampBundle, the object types ObReferenceObjectByHandle checks against...). Each is created in the kernel heap
// the first time its address is asked for, filled as a retail console's, and stays there until Reset. The binder
// resolves a title's variable imports with Address (layer/sys/ppc/exports.cpp lists the names as kVariables).
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "guest.h"

namespace any360::sys {

class KernelVariables {
 public:
  ~KernelVariables();
  // The variable's guest address, created on first use; 0 for a name the layer does not provide.
  uint32_t Address(Guest g, const std::string& name);
  // Forgets the variables (the kernel heap they live in is reset with the objects) and stops the clock.
  void Reset();
  // KeTimeStampBundle's interrupt time, system time and tick count, as the clock thread writes them every millisecond.
  void Tick(Guest g);

 private:
  uint32_t Create(Guest g, const std::string& name);
  std::mutex mutex_;
  std::map<std::string, uint32_t> addresses_;
  uint32_t time_stamp_bundle_ = 0;
  std::atomic<bool> ticking_{false};
  std::thread clock_;
};

KernelVariables& Variables();

}  // namespace any360::sys
