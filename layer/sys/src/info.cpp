#include "any360/sys/info.h"

namespace any360::sys {

uint32_t XGetAVPack() { return 6; }
uint32_t XGetGameRegion() { return 0xFFFF; }
uint32_t XGetLanguage() { return 1; }
uint32_t XamGetSystemVersion() { return 0; }

}  // namespace any360::sys
