// xboxkrnl.exe configuration and error exports: ExGetXConfigSetting (the console's settings a title can read) and
// RtlNtStatusToDosError (NTSTATUS to Win32 error codes).
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// The console settings the layer reports; defaults to the reference runtime's (English, country 103 = US).
struct ConsoleSettings {
  uint32_t language = 1;
  uint8_t country = 103;
};
ConsoleSettings& Console();

// NTSTATUS: writes the setting into buffer (buffer_size bytes) and its size into required_size when given.
uint32_t ExGetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size,
                             uint32_t required_size);
uint32_t RtlNtStatusToDosError(uint32_t status);

constexpr uint32_t kStatusSuccess = 0, kStatusBufferTooSmall = 0xC0000023, kStatusInvalidParameter1 = 0xC00000EF,
                   kStatusInvalidParameter2 = 0xC00000F0, kStatusInvalidParameter3 = 0xC00000F1;

}  // namespace any360::sys
