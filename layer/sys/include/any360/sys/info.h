// xam.xex system information exports: constants of the machine a title runs on. The values are the ones a retail
// console reports in the configuration the layer presents (NTSC-U region, VGA, English) and can be made settings later.
#pragma once

#include <cstdint>
#include <string>

#include "guest.h"

namespace any360::sys {

uint32_t XGetAVPack();         // 6: VGA
uint32_t XGetGameRegion();     // 0xFFFF: every region
uint32_t XGetLanguage();       // 1: English
uint32_t XamGetSystemVersion();

// Locales, as XAM's tables give them (the reference's): the console's country (ConsoleSettings) gives the locale, or
// the game region does when it has none. The string forms write big-endian UTF-16 with a terminator: E_INVALIDARG for
// a length of 2^31 or more, E_NOTFOUND (0x80070490) for an unknown id, ERROR_INSUFFICIENT_BUFFER as an HRESULT
// when the string does not fit.
uint32_t XamGetLocale();
uint32_t XamGetLocaleEx(uint32_t max_country, uint32_t max_locale);
uint32_t XamGetOnlineCountryFromLocale(uint32_t id);
uint32_t XamGetLocaleFromOnlineCountry(uint32_t id);
uint32_t XamGetLanguageFromOnlineLanguage(uint32_t id);
uint32_t XamGetCountryFromOnlineCountry(uint32_t id);
uint32_t XamGetOnlineCountryString(Guest g, uint32_t id, uint32_t length, uint32_t buffer);
uint32_t XamGetCountryString(Guest g, uint32_t id, uint32_t length, uint32_t buffer);
uint32_t XamGetLanguageString(Guest g, uint32_t id, uint32_t length, uint32_t buffer);
uint32_t XamGetLocaleString(Guest g, uint32_t id, uint32_t length, uint32_t buffer);
uint32_t XamGetOnlineLanguageString(Guest g, uint32_t id, uint32_t length, uint32_t buffer);
uint32_t XamGetLanguageLocaleString(Guest g, uint32_t language, uint32_t locale, uint32_t length, uint32_t buffer);
uint32_t XamGetOnlineLanguageAndCountryString(Guest g, uint32_t language, uint32_t country, uint32_t length, uint32_t buffer);
// A FILETIME in the host's time zone as "MM/DD/YYYY" or "HH:MM" (the reference's US forms), truncated to `count`
// characters, the buffer cleared first.
void XamFormatDateString(Guest g, uint64_t filetime, uint32_t buffer, uint32_t count);
void XamFormatTimeString(Guest g, uint64_t filetime, uint32_t buffer, uint32_t count);
// Resource locators: "file://media:/<container>.xzp#<resource>" for no module (the reference's local files),
// "section://<module in hex>,<container>#<resource>" otherwise; the system ones use the gamercrd, shrdres and xam
// containers.
uint32_t XamBuildResourceLocator(Guest g, uint64_t module, uint32_t container, uint32_t resource, uint32_t buffer, uint32_t count);
uint32_t XamBuildResourceLocator(Guest g, uint64_t module, const std::u16string& container, const std::u16string& resource,
                                 uint32_t buffer, uint32_t count);
uint32_t XamBuildSystemResourceLocator(Guest g, const char16_t* container, uint32_t file, uint32_t buffer, uint32_t count);

}  // namespace any360::sys
