// Guest memory as the system layer sees it: a flat big-endian address space behind a host base pointer. The layer
// never keeps host pointers to guest objects; every function takes guest addresses and the base, so the same code
// serves a static recompilation (stage 0/1), a decompiled title (stage 2) and a test harness.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace any360::sys {

struct Guest {
  uint8_t* base;

  uint8_t* host(uint32_t address) const { return base + address; }

  uint8_t Load8(uint32_t address) const { return base[address]; }
  void Store8(uint32_t address, uint8_t value) const { base[address] = value; }

  uint16_t Load16(uint32_t address) const {
    uint16_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return std::byteswap(value);
  }
  void Store16(uint32_t address, uint16_t value) const {
    value = std::byteswap(value);
    std::memcpy(base + address, &value, sizeof(value));
  }

  uint32_t Load32(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return std::byteswap(value);
  }
  void Store32(uint32_t address, uint32_t value) const {
    value = std::byteswap(value);
    std::memcpy(base + address, &value, sizeof(value));
  }

  uint64_t Load64(uint32_t address) const {
    uint64_t value;
    std::memcpy(&value, base + address, sizeof(value));
    return std::byteswap(value);
  }
  void Store64(uint32_t address, uint64_t value) const {
    value = std::byteswap(value);
    std::memcpy(base + address, &value, sizeof(value));
  }

  // Length of the NUL-terminated byte string at `address`.
  uint32_t StringLength(uint32_t address) const {
    return uint32_t(std::strlen(reinterpret_cast<const char*>(base + address)));
  }
  // Length in characters of the NUL-terminated 16-bit string at `address`.
  uint32_t WideLength(uint32_t address) const {
    uint32_t n = 0;
    while (Load16(address + 2 * n) != 0) ++n;
    return n;
  }
};

}  // namespace any360::sys
