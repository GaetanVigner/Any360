// xboxkrnl.exe run-time library exports (Rtl*): memory, strings, time. Guest addresses in, results as the kernel
// returns them; each function is verified equal to the reference runtime by layer/sys/tests/sys_io_tests.
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// ANSI_STRING / UNICODE_STRING as laid out in guest memory: length, maximum_length (bytes), pointer.
constexpr uint32_t kStringLengthOffset = 0, kStringMaximumLengthOffset = 2, kStringPointerOffset = 4;

// TIME_FIELDS as laid out in guest memory: eight 16-bit fields.
struct TimeFields {
  uint16_t year, month, day, hour, minute, second, milliseconds, weekday;
};
constexpr uint32_t kTimeFieldsSize = 16;
TimeFields LoadTimeFields(Guest g, uint32_t address);
void StoreTimeFields(Guest g, uint32_t address, const TimeFields& fields);

// 100 ns units since 1601-01-01 (FILETIME) <-> calendar fields. TimeFieldsToTime returns false when the fields are
// out of range (year before 1601, bad month or day, hour / minute / second / millisecond too large).
TimeFields TimeToTimeFields(uint64_t time);
bool TimeFieldsToTime(const TimeFields& fields, uint64_t& time);

uint32_t RtlCompareMemoryUlong(Guest g, uint32_t source, uint32_t length, uint32_t pattern);
void RtlFillMemoryUlong(Guest g, uint32_t destination, uint32_t length, uint32_t pattern);
uint32_t RtlUpperChar(uint32_t c);
uint32_t RtlLowerChar(uint32_t c);
uint32_t RtlCompareString(Guest g, uint32_t string1, uint32_t string2, uint32_t case_insensitive);
uint32_t RtlCompareStringN(Guest g, uint32_t string1, uint32_t length1, uint32_t string2, uint32_t length2,
                           uint32_t case_insensitive);
void RtlInitAnsiString(Guest g, uint32_t destination, uint32_t source);
void RtlInitUnicodeString(Guest g, uint32_t destination, uint32_t source);
uint32_t RtlMultiByteToUnicodeN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length);
uint32_t RtlUnicodeToMultiByteN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length);
void RtlTimeToTimeFields(Guest g, uint32_t time, uint32_t time_fields);
uint32_t RtlTimeFieldsToTime(Guest g, uint32_t time_fields, uint32_t time);
uint32_t RtlComputeCrc32(Guest g, uint32_t seed, uint32_t buffer, uint32_t length);
// The number of leading bytes that are equal.
uint32_t RtlCompareMemory(Guest g, uint32_t source1, uint32_t source2, uint32_t length);
// Structured exception handling is not provided yet: these do nothing, as the reference runtime does.
void RtlCaptureContext();
void RtlUnwind();
void __C_specific_handler();

}  // namespace any360::sys
