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
// Step 12 (Xenia's xboxkrnl_rtl.cc and xam_info.cc the source of each answer).
// RtlDowncaseUnicodeChar: RtlUpcaseUnicodeChar's mapping undone (the Basic Latin and Latin-1 capitals, U+0178 and
// U+039C), every other character itself. (Xenia calls the host's towlower, whose answer depends on its locale.)
uint32_t RtlDowncaseUnicodeChar(uint32_t c);
// RtlRandom: NT's random generator (the seed advanced twice through 0x7FFFFFED * seed + 0x7FFFFFC3 mod 0x7FFFFFFF,
// 32-bit arithmetic; the result taken from, and replaced in, a 128-entry table shared by the process), the seed stored
// back. The table starts as XAM's (Xenia's XamRtlRandomTable).
uint32_t RtlRandom(Guest g, uint32_t seed_ptr);
// RtlGetStackLimits: the current stack's end (low address) and base, from the PCR (r13): its alternate stack's when
// the PCR's flag at +0x6C says the thread runs on it.
void RtlGetStackLimits(Guest g, uint32_t pcr, uint32_t end_out, uint32_t base_out);
// RtlImageDirectoryEntryToData: the address of a PE image's data directory entry (its size stored big-endian at
// `size_out`): the image's base plus the entry's address when the image is mapped, else the entry's file offset
// through the section holding it; 0 for no PE32 header, an entry past the directory, an empty one or no section. An
// odd base is taken as a file (not mapped), rounded down.
uint32_t RtlImageDirectoryEntryToData(Guest g, uint32_t base, uint32_t mapped_as_image, uint32_t entry, uint32_t size_out);
// The number of leading bytes that are equal.
uint32_t RtlCompareMemory(Guest g, uint32_t source1, uint32_t source2, uint32_t length);
// Structured exception handling is not provided yet: these do nothing, as the reference runtime does.
void RtlCaptureContext();
void RtlUnwind();
void __C_specific_handler();

}  // namespace any360::sys
