// The C run-time formatting the kernel exports (sprintf, _snprintf, swprintf, _snwprintf, vsprintf, _vsnprintf,
// vswprintf, _vsnwprintf, _vscwprintf) and DbgPrint: the Xbox CRT's printf dialect (flags - + 0 space #, width and
// precision with *, sizes h l ll L w I32 I64, conversions c C d i o u x X e E f g G a A n p s S), formatting strings and
// arguments that live in guest memory. Arguments come from an ArgumentSource: the caller's registers and stack for the
// variadic forms, a va_list of 8-byte big-endian slots for the v* forms.
#pragma once

#include <cstdint>
#include <string>

#include "guest.h"

namespace any360::sys {

struct ArgumentSource {
  virtual ~ArgumentSource() = default;
  virtual uint64_t Next64() = 0;
  uint32_t Next32() { return uint32_t(Next64()); }
};

// A va_list: consecutive 8-byte big-endian slots at `address`.
class ArrayArguments : public ArgumentSource {
 public:
  ArrayArguments(Guest g, uint32_t address) : g_(g), address_(address) {}
  uint64_t Next64() override {
    const uint64_t value = g_.Load64(address_);
    address_ += 8;
    return value;
  }

 private:
  Guest g_;
  uint32_t address_;
};

// What a formatting call did: the characters it produced (narrow or wide) and the printf result, which is the number
// of characters or -1 when the format is invalid or a character cannot be represented.
struct Formatted {
  int32_t count = -1;
  std::string narrow;
  std::u16string wide;
};

// Formats the NUL-terminated format string at `format` (bytes, or big-endian 16-bit units when `wide`).
Formatted Format(Guest g, uint32_t format, ArgumentSource& args, bool wide);

// The exports. Truncation: sprintf / vsprintf write everything; the _sn* forms stop at `capacity` characters, the
// variadic one returning -1 and the v* one the full count when it did not fit; the output is NUL-terminated when there
// is room. A null buffer or format (or a capacity of 0 or less for the bounded forms) returns -1.
int32_t Sprintf(Guest g, uint32_t buffer, uint32_t format, ArgumentSource& args, bool wide);
int32_t Snprintf(Guest g, uint32_t buffer, int32_t capacity, uint32_t format, ArgumentSource& args, bool wide,
                 bool count_when_truncated);
int32_t Vscwprintf(Guest g, uint32_t format, ArgumentSource& args);
// DbgPrint: formats, hands the text to the debug sink when one is set, returns STATUS_SUCCESS.
uint32_t DbgPrint(Guest g, uint32_t format, ArgumentSource& args);
void SetDebugPrintSink(void (*sink)(const char* text, size_t length));

}  // namespace any360::sys
