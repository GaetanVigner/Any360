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
// The source string's characters into the destination's buffer, up to its maximum length; a null source empties it.
void RtlCopyString(Guest g, uint32_t destination, uint32_t source);
void RtlCopyUnicodeString(Guest g, uint32_t destination, uint32_t source);
// The buffer of a string the kernel allocated back to the pool; the string emptied.
void RtlFreeUnicodeString(Guest g, uint32_t string);
// The PE header of an image in memory (an "MZ" header whose e_lfanew, little-endian, leads to "PE\0\0"), else 0.
uint32_t RtlImageNtHeader(Guest g, uint32_t module);
void RtlInitUnicodeString(Guest g, uint32_t destination, uint32_t source);
uint32_t RtlMultiByteToUnicodeN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length);
uint32_t RtlUnicodeToMultiByteN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length);
void RtlTimeToTimeFields(Guest g, uint32_t time, uint32_t time_fields);
uint32_t RtlTimeFieldsToTime(Guest g, uint32_t time_fields, uint32_t time);
uint32_t RtlComputeCrc32(Guest g, uint32_t seed, uint32_t buffer, uint32_t length);
// UNICODE_STRING to ANSI_STRING with NT semantics: characters above 0xFF become '?', the length is set; with `allocate`
// the buffer comes from the kernel pool (RtlFreeAnsiString gives it back), otherwise STATUS_BUFFER_OVERFLOW when the
// destination is too small (it then holds what fits).
uint32_t RtlUnicodeStringToAnsiString(Guest g, uint32_t destination, uint32_t source, uint32_t allocate);
void RtlFreeAnsiString(Guest g, uint32_t string);
// Upper case for the Basic Latin and Latin-1 ranges (the console's table beyond them maps characters to themselves).
uint32_t RtlUpcaseUnicodeChar(uint32_t c);
// The number of leading bytes that are equal.
uint32_t RtlCompareMemory(Guest g, uint32_t source1, uint32_t source2, uint32_t length);
// Structured exception handling is not provided yet: these do nothing, as the reference runtime does.
void RtlCaptureContext();
void RtlUnwind();
void __C_specific_handler();

}  // namespace any360::sys
