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

// The value a setting is kept in, and its size (4 or 1 bytes); null for a setting the layer does not keep.
struct SettingField {
  uint32_t* word = nullptr;
  uint8_t* byte = nullptr;
  uint32_t size() const { return word ? 4 : byte ? 1 : 0; }
};
SettingField FieldOf(uint32_t category, uint32_t setting) {
  ConsoleSettings& s = Console();
  if (category == 0x0002) return setting == 0x0002 ? SettingField{&s.av_region, nullptr} : SettingField{};
  if (category != 0x0003) return {};
  switch (setting) {
    case 0x0001: case 0x0002: case 0x0003: case 0x0004: case 0x0005: case 0x0006: case 0x0007:
      return {&s.time_zone[setting - 1], nullptr};
    case 0x0009: return {&s.language, nullptr};
    case 0x000A: return {&s.video_flags, nullptr};
    case 0x000B: return {&s.audio_flags, nullptr};
    case 0x000C: return {&s.retail_flags, nullptr};
    case 0x000E: return {nullptr, &s.country};
    case 0x0019: return {nullptr, &s.pc_flags};
    default: return {};
  }
}

// The lookup proper; `size` is set only on success, as the kernel reports 0 for a setting it could not read.
uint32_t GetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t buffer_size,
                           uint32_t& required) {
  category &= 0xFFFF;
  setting &= 0xFFFF;
  buffer_size &= 0xFFFF;
  // XCONFIG_SECURED_CATEGORY (2) and XCONFIG_USER_CATEGORY (3); another category, or a setting of theirs the layer
  // does not keep, is refused by parameter.
  if (category != 0x0002 && category != 0x0003) return kStatusInvalidParameter1;
  const SettingField field = FieldOf(category, setting);
  const uint32_t size = field.size();
  if (!size) return kStatusInvalidParameter2;
  const uint32_t value = field.word ? *field.word : *field.byte;
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

uint32_t ExSetXConfigSetting(Guest g, uint32_t category, uint32_t setting, uint32_t buffer, uint32_t) {
  if (!buffer) return kStatusInvalidParameter3;
  const SettingField field = FieldOf(category & 0xFFFF, setting & 0xFFFF);
  if (field.word) *field.word = g.Load32(buffer);
  if (field.byte) *field.byte = g.Load8(buffer);
  return kStatusSuccess;
}

uint32_t ExReadModifyWriteXConfigSettingUlong(uint32_t category, uint32_t setting, uint32_t kept, uint32_t set) {
  const SettingField field = FieldOf(category & 0xFFFF, setting & 0xFFFF);
  if (field.word) *field.word = (*field.word & kept) | set;
  if (field.byte) *field.byte = uint8_t((*field.byte & kept) | set);
  return kStatusSuccess;
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
