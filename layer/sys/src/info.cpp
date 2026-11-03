#include "any360/sys/info.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "any360/sys/config.h"

namespace any360::sys {

uint32_t XGetAVPack() { return 6; }
uint32_t XGetGameRegion() { return 0xFFFF; }
uint32_t XGetLanguage() { return 1; }
uint32_t XamGetSystemVersion() { return 0; }

// --- locales: the XAM tables between console countries, online countries, languages and locales ---

namespace {

constexpr uint8_t kOnlineCountryFromLocale[] = {
    2, 6, 5, 8, 13, 16, 19, 20, 21, 23, 25, 32, 34, 24, 37, 39, 42, 46, 44, 50, 53, 56, 71, 74,
    76, 75, 82, 84, 91, 93, 109, 31, 90, 18, 101, 35, 103, 88, 236, 99, 4, 89, 45, 1,
};
constexpr const char* kOnlineCountryStrings[] = {
    "ZZ", "AE", "AL", "AM", "AR", "AT", "AU", "AZ", "BE", "BG", "BH", "BN", "BO", "BR", "BY", "BZ",
    "CA", nullptr, "CH", "CL", "CN", "CO", "CR", "CZ", "DE", "DK", "DO", "DZ", "EC", "EE", "EG", "ES",
    "FI", "FO", "FR", "GB", "GE", "GR", "GT", "HK", "HN", "HR", "HU", "ID", "IE", "IL", "IN", "IQ",
    "IR", "IS", "IT", "JM", "JO", "JP", "KE", "KG", "KR", "KW", "KZ", "LB", "LI", "LT", "LU", "LV",
    "LY", "MA", "MC", "MK", "MN", "MO", "MV", "MX", "MY", "NI", "NL", "NO", "NZ", "OM", "PA", "PE",
    "PH", "PK", "PL", "PR", "PT", "PY", "QA", "RO", "RU", "SA", "SE", "SG", "SI", "SK", nullptr, "SV",
    "SY", "TH", "TN", "TR", "TT", "TW", "UA", "US", "UY", "UZ", "VE", "VN", "YE", "ZA", "ZW", "AF",
    nullptr, "AD", "AO", "AI", nullptr, "AG", "AW", "BS", "BD", "BB", "BJ", "BM", "BT", "BA", "BW", "BF",
    "BI", "KH", "CM", "CV", "KY", "CF", "TD", "CX", "CC", "KM", "CG", "CD", "CK", "CI", "CY", "DJ",
    "DM", nullptr, "GQ", "ER", "ET", "FK", "FJ", "GF", "PF", "GA", "GM", "GH", "GI", "GL", "GD", "GP",
    nullptr, "GG", "GN", "GW", "GY", "HT", "JE", "KI", "LA", "LS", "LR", "MG", "MW", "ML", "MT", "MH",
    "MQ", "MR", "MU", "YT", "FM", "MD", "ME", "MS", "MZ", "MM", "NA", "NR", "NP", "AN", "NC", "NE",
    "NG", "NU", "NF", nullptr, "PW", "PS", "PG", "PN", "RE", "RW", "WS", "SM", "ST", "SN", "RS", "SC",
    "SL", "SB", "SO", "LK", "SH", "KN", "LC", "PM", "VC", "SR", "SZ", "TJ", "TZ", "TL", "TG", "TK",
    "TO", "TM", "TC", "TV", "UG", "VU", "VA", nullptr, "VG", "WF", "EH", "ZM", "ZZ",
};
constexpr const char* kCountryStrings[] = {
    "ZZ", "AE", "AL", "AM", "AR", "AT", "AU", "AZ", "BE", "BG", "BH", "BN", "BO", "BR", "BY", "BZ",
    "CA", nullptr, "CH", "CL", "CN", "CO", "CR", "CZ", "DE", "DK", "DO", "DZ", "EC", "EE", "EG", "ES",
    "FI", "FO", "FR", "GB", "GE", "GR", "GT", "HK", "HN", "HR", "HU", "ID", "IE", "IL", "IN", "IQ",
    "IR", "IS", "IT", "JM", "JO", "JP", "KE", "KG", "KR", "KW", "KZ", "LB", "LI", "LT", "LU", "LV",
    "LY", "MA", "MC", "MK", "MN", "MO", "MV", "MX", "MY", "NI", "NL", "NO", "NZ", "OM", "PA", "PE",
    "PH", "PK", "PL", "PR", "PT", "PY", "QA", "RO", "RU", "SA", "SE", "SG", "SI", "SK", nullptr, "SV",
    "SY", "TH", "TN", "TR", "TT", "TW", "UA", "US", "UY", "UZ", "VE", "VN", "YE", "ZA", "ZW", "ZZ",
};
constexpr const char* kLanguageStrings[] = {
    "zz", "en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt", nullptr, "pl", "ru", "sv", "tr", "nb",
    "nl", "zh",
};
constexpr const char* kLocaleStrings[] = {
    "ZZ", "AU", "AT", "BE", "BR", "CA", "CL", "CN", "CO", "CZ", "DK", "FI", "FR", "DE", "GR", "HK",
    "HU", "IN", "IE", "IT", "JP", "KR", "MX", "NL", "NZ", "NO", "PL", "PT", "SG", "SK", "ZA", "ES",
    "SE", "CH", "TW", "GB", "US", "RU", "ZZ", "TR", "AR", "SA", "IL", "AE",
};
constexpr uint8_t kLocaleFromOnlineCountry[] = {
    0, 43, 0, 0, 40, 2, 1, 0, 3, 0, 0, 0, 0, 4, 0, 0, 5, 0, 33, 6, 7, 8, 0, 9,
    13, 10, 0, 0, 0, 0, 0, 31, 11, 0, 12, 35, 0, 14, 0, 15, 0, 0, 16, 0, 18, 42, 17, 0,
    0, 0, 19, 0, 0, 20, 0, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 22,
    0, 0, 23, 25, 24, 0, 0, 0, 0, 0, 26, 0, 27, 0, 0, 0, 37, 41, 32, 28, 0, 29, 0, 0,
    0, 0, 0, 39, 0, 34, 0, 36, 0, 0, 0, 0, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 38,
};
constexpr uint8_t kLanguageFromOnlineLanguage[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 11, 12, 1, 1, 15, 16, 13, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 14, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
constexpr const char* kOnlineLanguageStrings[] = {
    "zz", "en", "ja", "de", "fr", "es", "it", "ko", "zh", "pt", "zh", "pl", "ru", "da", "fi", "nb",
    "nl", "sv", "cs", "el", "hu", "sk", "id", "ms", "ar", "bg", "et", "hr", "he", "is", "kk", "lt",
    "lv", "ro", "sl", "th", "tr", "uk", "vi", "ps", "sq", "hy", "bn", "be", "km", "am", "fo", "ka",
    "kl", "sw", "ky", "lb", "mk", "mt", "mn", "ne", "ur", "rw", "wo", "si", "tk",
};
constexpr uint8_t kCountryFromOnlineCountry[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
    72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 0, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 111,
};
constexpr uint8_t kLocaleFromCountry[] = {
    0, 43, 0, 0, 40, 2, 1, 0, 3, 0, 0, 0, 0, 4, 0, 0, 5, 0, 33, 6, 7, 8, 0, 9,
    13, 10, 0, 0, 0, 0, 0, 31, 11, 0, 12, 35, 0, 14, 0, 15, 0, 0, 16, 0, 18, 42, 17, 0,
    0, 0, 19, 0, 0, 20, 0, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 22,
    0, 0, 23, 25, 24, 0, 0, 0, 0, 0, 26, 0, 27, 0, 0, 0, 37, 41, 32, 28, 0, 29, 0, 0,
    0, 0, 0, 39, 0, 34, 0, 36, 0, 0, 0, 0, 0, 30, 0, 38,
};

template <typename T, size_t N>
T Lookup(const T (&table)[N], uint32_t id, T fallback) {
  const uint8_t index = uint8_t(id);
  return index < N ? table[index] : fallback;
}

constexpr uint32_t kEInvalidArg = 0x80070057, kENotFound = 0x80070490, kEInsufficientBuffer = 0x8007007A;

// The string as big-endian UTF-16 with its terminator, when it fits in `length` characters.
uint32_t StoreString(Guest g, const std::string& value, uint32_t length, uint32_t buffer) {
  if (value.size() + 1 > length) return kEInsufficientBuffer;
  for (size_t i = 0; i < value.size(); ++i) g.Store16(buffer + 2 * uint32_t(i), uint8_t(value[i]));
  g.Store16(buffer + 2 * uint32_t(value.size()), 0);
  return 0;
}

uint32_t StoreTableString(Guest g, const char* value, uint32_t length, uint32_t buffer) {
  if (length >= 0x80000000u) return kEInvalidArg;
  if (!value) return kENotFound;
  return StoreString(g, value, length, buffer);
}

uint32_t StorePair(Guest g, const char* first, const char* second, uint32_t length, uint32_t buffer) {
  if (length >= 0x80000000u) return kEInvalidArg;
  if (!first || !second) return kENotFound;
  return StoreString(g, std::string(first) + "-" + second, length, buffer);
}

// Up to count - 1 characters as big-endian UTF-16, then a terminator (nothing for a count of 0).
void StoreTruncated(Guest g, const std::u16string& value, uint32_t buffer, uint32_t count) {
  if (!count) return;
  const size_t n = std::min<size_t>(value.size(), count - 1);
  for (size_t i = 0; i < n; ++i) g.Store16(buffer + 2 * uint32_t(i), uint16_t(value[i]));
  g.Store16(buffer + 2 * uint32_t(n), 0);
}

std::u16string Widen(const std::string& s) { return std::u16string(s.begin(), s.end()); }

std::u16string GuestWide(Guest g, uint32_t string) {
  std::u16string s;
  if (!string) return s;
  for (uint32_t at = string; uint16_t c = g.Load16(at); at += 2) s += char16_t(c);
  return s;
}

// A FILETIME in the host's local time zone.
std::tm LocalTime(uint64_t filetime) {
  const auto since_epoch = std::chrono::nanoseconds((int64_t(filetime) - 116444736000000000ll) * 100);
  const std::chrono::sys_seconds utc = std::chrono::floor<std::chrono::seconds>(std::chrono::sys_time<std::chrono::nanoseconds>(since_epoch));
  const auto local = std::chrono::current_zone()->to_local(utc);
  const auto days = std::chrono::floor<std::chrono::days>(local);
  const std::chrono::year_month_day date{days};
  const std::chrono::hh_mm_ss time{local - days};
  std::tm t{};
  t.tm_year = int(date.year()) - 1900;
  t.tm_mon = int(unsigned(date.month())) - 1;
  t.tm_mday = int(unsigned(date.day()));
  t.tm_hour = int(time.hours().count());
  t.tm_min = int(time.minutes().count());
  return t;
}

}  // namespace

uint32_t XamGetLocaleEx(uint32_t max_country, uint32_t max_locale) {
  const uint8_t country = Console().country;
  if (country <= uint8_t(max_country)) {
    const uint8_t locale = Lookup(kLocaleFromCountry, country, uint8_t(0));
    if (locale <= uint8_t(max_locale)) return locale;
  }
  // No locale for the country: one from the game region.
  const uint32_t region = XGetGameRegion();
  if ((region & 0xFF00) >> 8 == 1) return region == 0x101 ? 20 /* JP */ : 21 /* KR */;
  return (region & 0xFF00) >> 8 == 2 ? 35 /* GB */ : 36 /* US */;
}

uint32_t XamGetLocale() { return XamGetLocaleEx(111, 43); }
uint32_t XamGetOnlineCountryFromLocale(uint32_t id) { return Lookup(kOnlineCountryFromLocale, id, uint8_t(0)); }
uint32_t XamGetLocaleFromOnlineCountry(uint32_t id) { return Lookup(kLocaleFromOnlineCountry, id, uint8_t(0)); }
uint32_t XamGetLanguageFromOnlineLanguage(uint32_t id) { return Lookup(kLanguageFromOnlineLanguage, id, uint8_t(0)); }
uint32_t XamGetCountryFromOnlineCountry(uint32_t id) { return Lookup(kCountryFromOnlineCountry, id, uint8_t(0)); }

uint32_t XamGetOnlineCountryString(Guest g, uint32_t id, uint32_t length, uint32_t buffer) {
  return StoreTableString(g, Lookup(kOnlineCountryStrings, id, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetCountryString(Guest g, uint32_t id, uint32_t length, uint32_t buffer) {
  return StoreTableString(g, Lookup(kCountryStrings, id, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetLanguageString(Guest g, uint32_t id, uint32_t length, uint32_t buffer) {
  return StoreTableString(g, Lookup(kLanguageStrings, id, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetLocaleString(Guest g, uint32_t id, uint32_t length, uint32_t buffer) {
  return StoreTableString(g, Lookup(kLocaleStrings, id, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetOnlineLanguageString(Guest g, uint32_t id, uint32_t length, uint32_t buffer) {
  return StoreTableString(g, Lookup(kOnlineLanguageStrings, id, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetLanguageLocaleString(Guest g, uint32_t language, uint32_t locale, uint32_t length, uint32_t buffer) {
  return StorePair(g, Lookup(kLanguageStrings, language, static_cast<const char*>(nullptr)),
                   Lookup(kLocaleStrings, locale, static_cast<const char*>(nullptr)), length, buffer);
}
uint32_t XamGetOnlineLanguageAndCountryString(Guest g, uint32_t language, uint32_t country, uint32_t length, uint32_t buffer) {
  return StorePair(g, Lookup(kOnlineLanguageStrings, language, static_cast<const char*>(nullptr)),
                   Lookup(kOnlineCountryStrings, country, static_cast<const char*>(nullptr)), length, buffer);
}

void XamFormatDateString(Guest g, uint64_t filetime, uint32_t buffer, uint32_t count) {
  std::memset(g.host(buffer), 0, size_t(count) * 2);
  const std::tm t = LocalTime(filetime);
  char text[32];
  std::snprintf(text, sizeof text, "%02d/%02d/%d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
  StoreTruncated(g, Widen(text), buffer, count);
}

void XamFormatTimeString(Guest g, uint64_t filetime, uint32_t buffer, uint32_t count) {
  std::memset(g.host(buffer), 0, size_t(count) * 2);
  const std::tm t = LocalTime(filetime);
  char text[16];
  std::snprintf(text, sizeof text, "%02d:%02d", t.tm_hour, t.tm_min);
  StoreTruncated(g, Widen(text), buffer, count);
}

uint32_t XamBuildResourceLocator(Guest g, uint64_t module, const std::u16string& container, const std::u16string& resource,
                                 uint32_t buffer, uint32_t count) {
  std::u16string path;
  if (!module) {
    path = u"file://media:/" + container + u".xzp#" + resource;
  } else {
    char hex[16];
    std::snprintf(hex, sizeof hex, "%X", uint32_t(module));
    path = u"section://" + Widen(hex) + u"," + container + u"#" + resource;
  }
  StoreTruncated(g, path, buffer, count);
  return 0;
}

uint32_t XamBuildResourceLocator(Guest g, uint64_t module, uint32_t container, uint32_t resource, uint32_t buffer, uint32_t count) {
  return XamBuildResourceLocator(g, module, GuestWide(g, container), GuestWide(g, resource), buffer, count);
}

uint32_t XamBuildSystemResourceLocator(Guest g, const char16_t* container, uint32_t file, uint32_t buffer, uint32_t count) {
  return XamBuildResourceLocator(g, 0, container, GuestWide(g, file), buffer, count);
}

}  // namespace any360::sys
