#include "any360/sys/config.h"

namespace any360::sys {

struct ErrorTable {
  uint32_t base;
  uint32_t count;
  const uint32_t* entries;
};
extern const ErrorTable kErrorTables[];
extern const unsigned kErrorTableCount;

ConsoleSettings& Console() {
  static ConsoleSettings settings;
  return settings;
}

namespace {

// The lookup proper; `size` is set only on success, as the kernel reports 0 for a setting it could not read.
uint32_t GetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size,
                           uint32_t& required) {
  category &= 0xFFFF;
  setting &= 0xFFFF;
  buffer_size &= 0xFFFF;
  uint32_t size = 0, value = 0;
  switch (category) {
    case 0x0002:  // XCONFIG_SECURED_CATEGORY
      if (setting != 0x0002) return kStatusInvalidParameter2;
      size = 4;
      value = 0x00001000;  // XCONFIG_SECURED_AV_REGION: USA / Canada
      break;
    case 0x0003:  // XCONFIG_USER_CATEGORY
      switch (setting) {
        case 0x0001: case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x0006: case 0x0007:  // time zone
          size = 4;
          value = 0;
          break;
        case 0x0009:  // XCONFIG_USER_LANGUAGE
          size = 4;
          value = Console().language;
          break;
        case 0x000A:  // XCONFIG_USER_VIDEO_FLAGS
          size = 4;
          value = 0x00040000;
          break;
        case 0x000B:  // XCONFIG_USER_AUDIO_FLAGS
          size = 4;
          value = 0x00010001;
          break;
        case 0x000C:  // XCONFIG_USER_RETAIL_FLAGS
          size = 4;
          value = 0x40;
          break;
        case 0x000E:  // XCONFIG_USER_COUNTRY
          size = 1;
          value = Console().country;
          break;
        case 0x0019:  // XCONFIG_USER_PC_FLAGS: XBLAllowed | XBLMembershipCreationAllowed
          size = 1;
          value = 0x03;
          break;
        default:
          return kStatusInvalidParameter2;
      }
      break;
    default:
      return kStatusInvalidParameter1;
  }
  if (buffer) {
    if (buffer_size < size) return kStatusBufferTooSmall;
    if (size == 4) g.Store32(buffer, value);
    else g.Store8(buffer, uint8_t(value));
  } else if (buffer_size) {
    return kStatusInvalidParameter3;
  }
  required = size;
  return kStatusSuccess;
}

}  // namespace

uint32_t ExGetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size,
                             uint32_t required_size) {
  uint32_t required = 0;
  const uint32_t status = GetXConfigSetting(g, category, setting, buffer, buffer_size, required);
  if (required_size) g.Store16(required_size, uint16_t(required));
  return status;
}

uint32_t RtlNtStatusToDosError(uint32_t status) {
  if (!status || (status & 0x20000000)) return status;
  if ((status >> 16) == 0x8007) return status & 0xFFFF;
  if ((status & 0xF0000000) == 0xD0000000) status &= ~0x30000000u;
  for (unsigned t = 0; t < kErrorTableCount; ++t) {
    const ErrorTable& table = kErrorTables[t];
    if (status < table.base) break;
    const uint32_t index = status - table.base;
    if (index < table.count) {
      const uint32_t result = table.entries[index];
      if (!result) break;
      return result;
    }
  }
  if ((status >> 16) == 0xC001) return status & 0xFFFF;
  return 317;  // ERROR_MR_MID_NOT_FOUND
}

}  // namespace any360::sys
