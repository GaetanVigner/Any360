// xam.xex system information exports: constants of the machine a title runs on. The values are the ones a retail
// console reports in the configuration the layer presents (NTSC-U region, VGA, English) and can be made settings later.
#pragma once

#include <cstdint>

namespace any360::sys {

uint32_t XGetAVPack();         // 6: VGA
uint32_t XGetGameRegion();     // 0xFFFF: every region
uint32_t XGetLanguage();       // 1: English
uint32_t XamGetSystemVersion();

}  // namespace any360::sys
