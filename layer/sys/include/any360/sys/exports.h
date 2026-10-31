// The system calls the layer provides with the PowerPC calling convention (layer/sys/ppc/exports.cpp): a title binds
// each `function` in place of the reference runtime's `__imp__<name>`.
#pragma once

#include <cstdint>

struct PPCContext;

namespace any360::sys {

struct Export {
  const char* name;
  void (*function)(PPCContext& ctx, uint8_t* base);
};

extern const Export kExports[];
extern const unsigned kExportCount;

}  // namespace any360::sys
