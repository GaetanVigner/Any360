// xboxkrnl.exe configuration and error exports: ExGetXConfigSetting (the console's settings a title can read) and
// RtlNtStatusToDosError (NTSTATUS to Win32 error codes).
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// The console settings the layer reports; defaults to the reference runtime's (English, country 103 = US). A title
// may change them for the run (ExSetXConfigSetting, ExReadModifyWriteXConfigSettingUlong, XamSetCountry).
struct ConsoleSettings {
  uint32_t language = 1;
  uint8_t country = 103;
  uint32_t av_region = 0x00001000;   // XCONFIG_SECURED_AV_REGION: USA / Canada
  uint32_t time_zone[7] = {};        // XCONFIG_USER_TIME_ZONE_* (settings 1-7): UTC
  uint32_t video_flags = 0x00040000;  // XCONFIG_USER_VIDEO_FLAGS
  uint32_t audio_flags = 0x00010001;  // XCONFIG_USER_AUDIO_FLAGS
  uint32_t retail_flags = 0x40;       // XCONFIG_USER_RETAIL_FLAGS
  uint8_t pc_flags = 0x03;            // XCONFIG_USER_PC_FLAGS: XBLAllowed | XBLMembershipCreationAllowed
};
ConsoleSettings& Console();

// NTSTATUS: writes the setting into buffer (buffer_size bytes) and its size into required_size when given.
uint32_t ExGetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size,
                             uint32_t required_size);
// ExSetXConfigSetting (category, setting, buffer, size): the setting ExGetXConfigSetting reads takes the buffer's
// value (its own size: 4 or 1 bytes, big-endian), for the run. As Xenia, STATUS_SUCCESS whatever the size given, and
// for a setting the layer does not keep (nothing stored); STATUS_INVALID_PARAMETER_3 without a buffer.
uint32_t ExSetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size);
// ExReadModifyWriteXConfigSettingUlong (category, setting, bits kept, bits set): value = (value & kept) | set, as
// Xenia (a one-byte setting keeps the low byte); STATUS_SUCCESS, nothing stored for a setting the layer does not keep.
uint32_t ExReadModifyWriteXConfigSettingUlong(uint32_t category, uint32_t setting, uint32_t kept, uint32_t set);
uint32_t RtlNtStatusToDosError(uint32_t status);

constexpr uint32_t kStatusSuccess = 0, kStatusBufferTooSmall = 0xC0000023, kStatusInvalidParameter1 = 0xC00000EF,
                   kStatusInvalidParameter2 = 0xC00000F0, kStatusInvalidParameter3 = 0xC00000F1;

}  // namespace any360::sys
