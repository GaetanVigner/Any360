// xam.xex networking exports (NetDll_*) that need no socket state: address parsing and conversions, fd_set tests.
// With no network behind them, the XNet conversions answer as an offline console does.
#pragma once

#include <cstdint>

#include "guest.h"

namespace any360::sys {

// Winsock inet_addr on a NUL-terminated string: the address in host order (a.b.c.d -> 0xAABBCCDD), 0xFFFFFFFF when
// it does not parse, 0 for the empty string.
uint32_t NetDll_inet_addr(Guest g, uint32_t string);
// Writes a dotted string for the address into `out` (`size` bytes including the NUL).
void NetDll_XNetInAddrToString(Guest g, uint32_t in_addr, uint32_t out, uint32_t size);
uint32_t NetDll_XNetXnAddrToInAddr();
uint32_t NetDll_XNetInAddrToXnAddr();
// 1 when `socket` is one of the first min(fd_count, 64) handles of the fd_set.
uint32_t NetDll___WSAFDIsSet(Guest g, uint32_t socket, uint32_t fd_set);

}  // namespace any360::sys
