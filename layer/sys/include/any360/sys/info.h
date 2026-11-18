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

// Step 12, xam.xex's information calls Xenia implements (its xam_info.cc, xam_locale.cc and title_id_utils.h are the
// source of each answer).
// XGetAudioFlags: the console's audio flags; no audio setting is kept, so XAM's default (0x10001, stereo with Dolby
// Digital allowed) as Xenia answers it on a VGA pack.
uint32_t XGetAudioFlags();
// XamGetLanguage: the extended language (up to Simplified Chinese, 17): the console's language when it is one,
// else the game region's (Japanese for NTSC-J Japan, Simplified Chinese for NTSC-J China, Korean for the other
// NTSC-J regions, English everywhere else).
uint32_t XamGetLanguage();
// XamGetOnlineCountryFeatures: XAM's feature bits of an online country; 0x80000000 for one it does not list.
uint32_t XamGetOnlineCountryFeatures(uint32_t online_country);
// The title-id classes: system titles (id 0, the 0xFFFE system range outside the X publisher, every X publisher
// title except XBLA games), system experiences (the XH / XJ apps, Xbox Live's 2001 and 2002 titles), original Xbox
// titles (the 0xFFFE range, ids under 0x7D0 outside the 0xFF publishers), and the dashboard (0xFFFE07D1).
constexpr uint32_t kDashboardTitleId = 0xFFFE07D1;
bool XamIsSystemTitleId(uint32_t title_id);
bool XamIsSystemExperienceTitleId(uint32_t title_id);
bool XamIsXbox1TitleId(uint32_t title_id);
// XamGetLanguageTypeface: the locator of the font for a language (XenonSCLatin for Simplified Chinese, XenonCLatin
// for Traditional, XenonJKLatin for every other), as big-endian UTF-16 with its terminator, whatever the buffer size
// (as Xenia writes it). STATUS_SUCCESS.
uint32_t XamGetLanguageTypeface(Guest g, uint32_t language, uint32_t buffer_size, uint32_t buffer);
// The locale name a language falls back to ("ja-JP" for Japanese ... "zh-CHS" for Simplified Chinese; empty for
// English and an unknown language), as XamGetLanguageLocaleFallbackString lays it in guest memory.
const char16_t* LanguageLocaleFallback(uint32_t language);
// XamGetOnlineLanguageAndCountry (xuid, language byte out, country byte out): the profile's language and country,
// which are the console's here (one local profile); a null out is skipped.
void XamGetOnlineLanguageAndCountry(Guest g, uint32_t language, uint32_t country);

}  // namespace any360::sys
