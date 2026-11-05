// The system calls the layer provides with the PowerPC calling convention (layer/sys/ppc/exports.cpp): a title binds
// each `function` in place of the reference runtime's `__imp__<name>`. The variables it provides are listed apart: a
// title's import of one resolves to the guest address VariableAddress gives (KernelVariables, variables.h).
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

struct Variable {
  const char* name;
};

extern const Variable kVariables[];
extern const unsigned kVariableCount;

// The guest address of an exported variable (created on first use in the kernel heap the object manager has), or 0.
uint32_t VariableAddress(uint8_t* base, const char* name);

}  // namespace any360::sys
