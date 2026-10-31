#include "any360/sys/net.h"

#include <algorithm>
#include <cstring>

namespace any360::sys {

namespace {

// One component of a dotted address: decimal, 0x hexadecimal or 0 octal, as the BSD / Winsock parser reads it.
bool Component(const char*& p, uint32_t& value) {
  uint32_t base = 10;
  if (*p == '0') {
    ++p;
    if (*p == 'x' || *p == 'X') {
      base = 16;
      ++p;
    } else {
      base = 8;
    }
  }
  value = 0;
  bool digits = base != 10;  // "0" alone is a valid octal component
  for (;; ++p) {
    uint32_t d;
    if (*p >= '0' && *p <= '9') d = uint32_t(*p - '0');
    else if (base == 16 && *p >= 'a' && *p <= 'f') d = uint32_t(*p - 'a' + 10);
    else if (base == 16 && *p >= 'A' && *p <= 'F') d = uint32_t(*p - 'A' + 10);
    else break;
    if (d >= base) return false;
    value = value * base + d;
    digits = true;
  }
  return digits;
}

}  // namespace

uint32_t NetDll_inet_addr(Guest g, uint32_t string) {
  if (!string) return 0xFFFFFFFF;
  const char* p = reinterpret_cast<const char*>(g.host(string));
  if (!*p) return 0;
  uint32_t parts[4];
  int n = 0;
  for (;;) {
    if (n == 4 || !Component(p, parts[n])) return 0xFFFFFFFF;
    ++n;
    if (*p != '.') break;
    ++p;
  }
  // Trailing blanks are allowed; anything else is not.
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') ++p;
  if (*p) return 0xFFFFFFFF;
  uint32_t address = parts[n - 1];
  const uint32_t limit = n == 1 ? 0xFFFFFFFF : n == 2 ? 0xFFFFFF : n == 3 ? 0xFFFF : 0xFF;
  if (address > limit) return 0xFFFFFFFF;
  for (int i = 0; i < n - 1; ++i) {
    if (parts[i] > 0xFF) return 0xFFFFFFFF;
    address |= parts[i] << (24 - 8 * i);
  }
  return address;
}

void NetDll_XNetInAddrToString(Guest g, uint32_t, uint32_t out, uint32_t size) {
  static const char kOffline[] = "666.666.666.666";
  if (!size) return;
  const uint32_t n = std::min<uint32_t>(size - 1, sizeof(kOffline) - 1);
  std::memcpy(g.host(out), kOffline, n);
  g.Store8(out + n, 0);
}

uint32_t NetDll_XNetXnAddrToInAddr() { return 1; }
uint32_t NetDll_XNetInAddrToXnAddr() { return 1; }

uint32_t NetDll___WSAFDIsSet(Guest g, uint32_t socket, uint32_t fd_set) {
  const uint32_t count = std::min<uint32_t>(g.Load32(fd_set), 64);
  for (uint32_t i = 0; i < count; ++i) {
    if (g.Load32(fd_set + 4 + 4 * i) == socket) return 1;
  }
  return 0;
}

}  // namespace any360::sys
