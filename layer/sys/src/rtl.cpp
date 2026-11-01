#include "any360/sys/rtl.h"

#include "any360/sys/memory.h"

#include <algorithm>
#include <cstring>

namespace any360::sys {

namespace {

// Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm), and back.
void CivilFromDays(int64_t z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = unsigned(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = int64_t(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = int(y + (month <= 2));
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = unsigned(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + int64_t(doe) - 719468;
}

bool IsLeap(int64_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

unsigned DaysInMonth(int64_t y, unsigned m) {
  static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && IsLeap(y) ? 29 : kDays[m - 1];
}

constexpr int64_t kFileTimeEpochToUnixSeconds = 11644473600ll;  // 1601-01-01 -> 1970-01-01
constexpr int64_t kTicksPerSecond = 10'000'000;
constexpr int64_t kTicksPerDay = kTicksPerSecond * 86400;

int Lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// strcmp / strncmp as the C runtime returns them (-1, 0, 1); the case-insensitive forms return the difference of the
// first lowered characters that differ, as _stricmp / _strnicmp do.
int CompareBytes(const char* a, const char* b, size_t n, bool case_insensitive) {
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
    if (case_insensitive) {
      const int d = Lower(x) - Lower(y);
      if (d) return d;
      if (!x) return 0;
    } else {
      if (x != y) return x < y ? -1 : 1;
      if (!x) return 0;
    }
  }
  return 0;
}

uint32_t Crc32Table(unsigned index) {
  uint32_t c = index;
  for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  return c;
}

}  // namespace

TimeFields LoadTimeFields(Guest g, uint32_t a) {
  return {g.Load16(a), g.Load16(a + 2), g.Load16(a + 4), g.Load16(a + 6), g.Load16(a + 8), g.Load16(a + 10), g.Load16(a + 12), g.Load16(a + 14)};
}

void StoreTimeFields(Guest g, uint32_t a, const TimeFields& f) {
  const uint16_t values[] = {f.year, f.month, f.day, f.hour, f.minute, f.second, f.milliseconds, f.weekday};
  for (uint32_t i = 0; i < 8; ++i) g.Store16(a + 2 * i, values[i]);
}

TimeFields TimeToTimeFields(uint64_t time) {
  const int64_t ticks = int64_t(time);
  int64_t days = ticks / kTicksPerDay, rest = ticks % kTicksPerDay;
  if (rest < 0) {
    rest += kTicksPerDay;
    --days;
  }
  const int64_t unix_days = days - kFileTimeEpochToUnixSeconds / 86400;
  int year;
  unsigned month, day;
  CivilFromDays(unix_days, year, month, day);
  TimeFields f{};
  f.year = uint16_t(year);
  f.month = uint16_t(month);
  f.day = uint16_t(day);
  // 1970-01-01 was a Thursday (4); weekday counts from Sunday.
  f.weekday = uint16_t(((unix_days % 7) + 7 + 4) % 7);
  const int64_t ms = rest / 10'000;
  f.hour = uint16_t(ms / 3'600'000);
  f.minute = uint16_t(ms / 60'000 % 60);
  f.second = uint16_t(ms / 1'000 % 60);
  f.milliseconds = uint16_t(ms % 1'000);
  return f;
}

bool TimeFieldsToTime(const TimeFields& f, uint64_t& time) {
  if (f.year < 1601 || f.month < 1 || f.month > 12 || f.day < 1 || f.day > 31 || f.hour > 23 || f.minute > 59 ||
      f.second > 59 || f.milliseconds > 999) {
    return false;
  }
  if (f.day > DaysInMonth(f.year, f.month)) return false;
  const int64_t unix_days = DaysFromCivil(f.year, f.month, f.day);
  const int64_t seconds = (unix_days + kFileTimeEpochToUnixSeconds / 86400) * 86400 + f.hour * 3600 + f.minute * 60 + f.second;
  time = uint64_t(seconds * kTicksPerSecond + int64_t(f.milliseconds) * 10'000);
  return true;
}

uint32_t RtlCompareMemoryUlong(Guest g, uint32_t source, uint32_t length, uint32_t pattern) {
  if (source % 4 || length % 4) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < length / 4; ++i) {
    if (g.Load32(source + 4 * i) == pattern) ++n;
  }
  return n;
}

void RtlFillMemoryUlong(Guest g, uint32_t destination, uint32_t length, uint32_t pattern) {
  for (uint32_t i = 0; i < length >> 2; ++i) g.Store32(destination + 4 * i, pattern);
}

// The kernel works on a signed char: bytes above 0x7F come back sign-extended.
uint32_t RtlUpperChar(uint32_t in) {
  const char c = char(in & 0xFF);
  return uint32_t(int32_t(c >= 'a' && c <= 'z' ? char(c ^ 0x20) : c));
}

uint32_t RtlLowerChar(uint32_t in) {
  const char c = char(in & 0xFF);
  return uint32_t(int32_t(c >= 'A' && c <= 'Z' ? char(c ^ 0x20) : c));
}

uint32_t RtlCompareString(Guest g, uint32_t string1, uint32_t string2, uint32_t case_insensitive) {
  const char* a = reinterpret_cast<const char*>(g.host(string1));
  const char* b = reinterpret_cast<const char*>(g.host(string2));
  return uint32_t(int32_t(CompareBytes(a, b, SIZE_MAX, case_insensitive != 0)));
}

uint32_t RtlCompareStringN(Guest g, uint32_t string1, uint32_t length1, uint32_t string2, uint32_t length2,
                           uint32_t case_insensitive) {
  const char* a = reinterpret_cast<const char*>(g.host(string1));
  const char* b = reinterpret_cast<const char*>(g.host(string2));
  const uint32_t len1 = length1 == 0xFFFF ? uint32_t(std::strlen(a)) : length1;
  const uint32_t len2 = length2 == 0xFFFF ? uint32_t(std::strlen(b)) : length2;
  const uint32_t len = std::min(len1, len2);
  int ret = CompareBytes(a, b, len, case_insensitive != 0);
  if (ret == 0) ret = int(len1) - int(len2);
  return uint32_t(int32_t(ret));
}

void RtlInitAnsiString(Guest g, uint32_t destination, uint32_t source) {
  if (source) {
    const uint16_t length = uint16_t(g.StringLength(source));
    g.Store16(destination + kStringLengthOffset, length);
    g.Store16(destination + kStringMaximumLengthOffset, uint16_t(length + 1));
  } else {
    g.Store16(destination + kStringLengthOffset, 0);
    g.Store16(destination + kStringMaximumLengthOffset, 0);
  }
  g.Store32(destination + kStringPointerOffset, source);
}

void RtlInitUnicodeString(Guest g, uint32_t destination, uint32_t source) {
  if (source) {
    const uint32_t n = g.WideLength(source);
    g.Store16(destination + kStringLengthOffset, uint16_t(n * 2));
    g.Store16(destination + kStringMaximumLengthOffset, uint16_t((n + 1) * 2));
    g.Store32(destination + kStringPointerOffset, source);
  } else {
    g.Store16(destination + kStringLengthOffset, 0);
    g.Store16(destination + kStringMaximumLengthOffset, 0);
    g.Store32(destination + kStringPointerOffset, 0);
  }
}

uint32_t RtlMultiByteToUnicodeN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length) {
  const uint32_t n = std::min(destination_length >> 1, source_length);
  for (uint32_t i = 0; i < n; ++i) g.Store16(destination + 2 * i, g.Load8(source + i));
  if (written) g.Store32(written, n << 1);
  return 0;
}

uint32_t RtlUnicodeToMultiByteN(Guest g, uint32_t destination, uint32_t destination_length, uint32_t written,
                                uint32_t source, uint32_t source_length) {
  const uint32_t n = std::min(source_length >> 1, destination_length);
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t c = g.Load16(source + 2 * i);
    g.Store8(destination + i, c < 256 ? uint8_t(c) : '?');
  }
  if (written) g.Store32(written, n);
  return 0;
}

void RtlTimeToTimeFields(Guest g, uint32_t time, uint32_t time_fields) {
  StoreTimeFields(g, time_fields, TimeToTimeFields(g.Load64(time)));
}

uint32_t RtlTimeFieldsToTime(Guest g, uint32_t time_fields, uint32_t time) {
  uint64_t value;
  if (!TimeFieldsToTime(LoadTimeFields(g, time_fields), value)) return 0;
  g.Store64(time, value);
  return 1;
}

uint32_t RtlComputeCrc32(Guest g, uint32_t seed, uint32_t buffer, uint32_t length) {
  if (!length) return seed;
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (unsigned i = 0; i < 256; ++i) table[i] = Crc32Table(i);
    ready = true;
  }
  uint32_t hash = ~seed;
  for (uint32_t i = 0; i < length; ++i) hash = table[g.Load8(buffer + i) ^ (hash & 0xFF)] ^ (hash >> 8);
  return ~hash;
}

uint32_t RtlCompareMemory(Guest g, uint32_t source1, uint32_t source2, uint32_t length) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < length; ++i) {
    if (g.Load8(source1 + i) == g.Load8(source2 + i)) ++n;
  }
  return n;
}

uint32_t RtlUnicodeStringToAnsiString(Guest g, uint32_t destination, uint32_t source, uint32_t allocate) {
  const uint32_t chars = source ? g.Load16(source) / 2 : 0, from = source ? g.Load32(source + 4) : 0;
  if (chars > 0xFFFE) return 0xC00000F0;  // STATUS_INVALID_PARAMETER_2
  uint32_t buffer, capacity;
  if (allocate) {
    buffer = Memory().AllocatePages(g, chars + 1);
    if (!buffer) return 0xC0000017;  // STATUS_NO_MEMORY
    capacity = chars + 1;
    g.Store16(destination + 2, uint16_t(capacity));
    g.Store32(destination + 4, buffer);
  } else {
    capacity = g.Load16(destination + 2);
    buffer = g.Load32(destination + 4);
  }
  const uint32_t n = std::min(chars, capacity ? capacity - 1 : 0);
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t c = g.Load16(from + 2 * i);
    g.Store8(buffer + i, c < 0x100 ? uint8_t(c) : '?');
  }
  if (capacity) g.Store8(buffer + n, 0);
  g.Store16(destination, uint16_t(n));
  return n < chars ? 0x80000005 : 0;  // STATUS_BUFFER_OVERFLOW
}

void RtlFreeAnsiString(Guest g, uint32_t string) {
  if (const uint32_t buffer = g.Load32(string + 4)) Memory().FreePages(buffer);
  g.Store32(string, 0);
  g.Store32(string + 4, 0);
}

uint32_t RtlUpcaseUnicodeChar(uint32_t c) {
  c &= 0xFFFF;
  if (c >= 'a' && c <= 'z') return c - 0x20;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if (c == 0xB5) return 0x39C;
  return c;
}

void RtlCaptureContext() {}
void RtlUnwind() {}
void __C_specific_handler() {}

}  // namespace any360::sys
