// any360 shim for <rex/memory.h> (the ReXGlue memory umbrella: utils, arena, mapped memory, ring buffer, guest
// memory). The shader translator only needs the few header-only helpers below, reproduced from
// rex/memory/utils.h (BSD-3, see ../../../LICENSE) with copy_and_swap reduced to a portable loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::memory {

#ifndef REX_RESTRICT_VAR
#define REX_RESTRICT_VAR __restrict
#endif

template <typename Dst, typename Src>
void Reinterpret(Dst& REX_RESTRICT_VAR dst, const Src& REX_RESTRICT_VAR src) {
  static_assert(sizeof(Dst) == sizeof(Src));
  static_assert(std::is_trivially_copyable_v<Dst>);
  static_assert(std::is_trivially_copyable_v<Src>);
  std::memcpy(&dst, &src, sizeof(Dst));
}

template <typename Dst, typename Src>
Dst Reinterpret(const Src& REX_RESTRICT_VAR src) {
  Dst dst;
  Reinterpret(dst, src);
  return dst;
}

template <typename T>
void copy_and_swap(T* dest, const T* src, size_t count) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
  if constexpr (sizeof(T) == 1) {
    std::memcpy(dest, src, count);
  } else {
    using U = std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>;
    for (size_t i = 0; i < count; ++i) {
      U value;
      std::memcpy(&value, src + i, sizeof(U));
      value = rex::byte_swap(value);
      std::memcpy(dest + i, &value, sizeof(U));
    }
  }
}

using fourcc_t = uint32_t;

constexpr inline fourcc_t make_fourcc(char a, char b, char c, char d) {
  return fourcc_t((static_cast<fourcc_t>(a) << 24) | (static_cast<fourcc_t>(b) << 16) |
                  (static_cast<fourcc_t>(c) << 8) | static_cast<fourcc_t>(d));
}

constexpr inline fourcc_t make_fourcc(const std::string_view fourcc) {
  if (fourcc.length() != 4) {
    throw std::runtime_error("Invalid fourcc length");
  }
  return make_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

// Guest memory is not part of the translator; declared so headers naming it still parse.
class Memory;

}  // namespace rex::memory
