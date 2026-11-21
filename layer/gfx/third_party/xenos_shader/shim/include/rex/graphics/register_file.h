// any360 shim for <rex/graphics/register_file.h>: the register file the shader interpreter (interpreter.cpp)
// reads, as a view over a register array the caller keeps (any360::gfx's, reg::kRegisterCount dwords) instead of
// an owned copy. The accessors are the SDK's (BSD-3, see ../../../../LICENSE).
#pragma once

#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::graphics {

class RegisterFile {
 public:
  explicit RegisterFile(const uint32_t* values) : values_(values) {}

  const uint32_t& operator[](uint32_t reg) const { return values_[reg]; }

  template <typename T>
  T Get(uint32_t reg) const {
    return rex::memory::Reinterpret<T>(values_[reg]);
  }
  template <typename T>
  T Get(Register reg) const {
    return Get<T>(static_cast<uint32_t>(reg));
  }
  template <typename T>
  T Get() const {
    return Get<T>(T::register_index);
  }

  xenos::xe_gpu_vertex_fetch_t GetVertexFetch(uint32_t index) const {
    assert_true(index < 96);
    xenos::xe_gpu_vertex_fetch_t fetch;
    std::memcpy(&fetch, &values_[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (sizeof(fetch) / sizeof(uint32_t)) * index],
                sizeof(fetch));
    return fetch;
  }

 private:
  const uint32_t* values_;
};

}  // namespace rex::graphics
