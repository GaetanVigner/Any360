#include "any360/sys/format.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace any360::sys {

namespace {

void (*g_debug_sink)(const char*, size_t) = nullptr;

enum : uint32_t {
  kLeft = 1u << 0, kZeros = 1u << 1, kPlus = 1u << 2, kSpace = 1u << 3, kAlternate = 1u << 4,
  kShort = 1u << 5, kLong = 1u << 6, kLongLong = 1u << 7, kWideSize = 1u << 8, kInvertWide = 1u << 9,
};

// Reads the format string from guest memory, as bytes or as big-endian 16-bit units.
class Reader {
 public:
  Reader(Guest g, uint32_t address, bool wide) : g_(g), address_(address), wide_(wide) {}
  uint16_t Peek(uint32_t ahead = 0) const {
    return wide_ ? g_.Load16(address_ + 2 * ahead) : uint16_t(g_.Load8(address_ + ahead));
  }
  uint16_t Get() {
    const uint16_t c = Peek();
    if (c) address_ += wide_ ? 2 : 1;
    return c;
  }

 private:
  Guest g_;
  uint32_t address_;
  bool wide_;
};

struct Writer {
  bool wide;
  Formatted& out;
  bool Put(uint16_t c) {
    if (wide) {
      out.wide.push_back(char16_t(c));
    } else {
      if (c >= 0x100) return false;
      out.narrow.push_back(char(c));
    }
    return true;
  }
  bool Fill(uint16_t c, int32_t n) {
    while (n-- > 0) {
      if (!Put(c)) return false;
    }
    return true;
  }
};

// The text of one conversion: narrow or 16-bit characters from host or guest memory.
struct Piece {
  const uint8_t* narrow = nullptr;
  const uint16_t* wide = nullptr;  // big-endian when from guest memory
  bool wide_is_big_endian = true;
  int32_t length = 0;
  uint16_t At(int32_t i) const {
    if (narrow) return narrow[i];
    uint16_t c;
    std::memcpy(&c, wide + i, 2);
    return wide_is_big_endian ? uint16_t((c << 8) | (c >> 8)) : c;
  }
};

std::string FormatDouble(double value, int32_t precision, uint16_t c, bool alternate) {
  if (precision < 0) precision = 6;
  else if (precision == 0 && (c == 'g' || c == 'G')) precision = 1;
  char spec[16];
  char buffer[512];
  int n;
  if (c == 'a' || c == 'A') {
    // Hexadecimal floats ignore the precision: the full 13 hex digits of the mantissa.
    std::snprintf(spec, sizeof spec, "%%%s%c", alternate ? "#" : "", char(c));
    n = std::snprintf(buffer, sizeof buffer, spec, value);
  } else {
    std::snprintf(spec, sizeof spec, "%%%s.*%c", alternate ? "#" : "", char(c));
    n = std::snprintf(buffer, sizeof buffer, spec, precision, value);
  }
  return std::string(buffer, size_t(std::max(0, std::min(n, int(sizeof buffer) - 1))));
}

}  // namespace

Formatted Format(Guest g, uint32_t format, ArgumentSource& args, bool wide) {
  Formatted out;
  Reader in(g, format, wide);
  Writer w{wide, out};
  int32_t count = 0;
  char digits[512];
  uint16_t wide_char;
  for (;;) {
    uint16_t c = in.Get();
    if (!c) {
      out.count = count;
      return out;
    }
    if (c != '%') {
      if (!w.Put(c)) return out;
      ++count;
      continue;
    }
    c = in.Get();
    if (c == '%') {
      if (!w.Put('%')) return out;
      ++count;
      continue;
    }
    uint32_t flags = 0;
    int32_t width = 0, precision = -1;
    // Flags.
    for (;; c = in.Get()) {
      if (c == '-') flags |= kLeft;
      else if (c == '+') flags |= kPlus;
      else if (c == '0') flags |= kZeros;
      else if (c == ' ') flags |= kSpace;
      else if (c == '#') flags |= kAlternate;
      else break;
    }
    // Width.
    if (c == '*') {
      width = int32_t(args.Next32());
      if (width < 0) {
        flags |= kLeft;
        width = -width;
      }
      c = in.Get();
    } else {
      for (; c >= '0' && c <= '9'; c = in.Get()) width = width * 10 + (c - '0');
    }
    // Precision.
    if (c == '.') {
      precision = 0;
      c = in.Get();
      if (c == '*') {
        precision = int32_t(args.Next32());
        if (precision < 0) precision = -1;
        c = in.Get();
      } else {
        for (; c >= '0' && c <= '9'; c = in.Get()) precision = precision * 10 + (c - '0');
      }
    }
    // Size.
    if (c == 'l') {
      if (in.Peek() == 'l') {
        in.Get();
        flags |= kLongLong;
      } else {
        flags |= kLong;
      }
      c = in.Get();
    } else if (c == 'L') {
      c = in.Get();
    } else if (c == 'h') {
      flags |= kShort;
      c = in.Get();
    } else if (c == 'w') {
      flags |= kWideSize;
      c = in.Get();
    } else if (c == 'I') {
      if ((in.Peek() == '6' && in.Peek(1) == '4')) {
        in.Get();
        in.Get();
        flags |= kLongLong;
      } else if (in.Peek() == '3' && in.Peek(1) == '2') {
        in.Get();
        in.Get();
      }
      c = in.Get();
    }
    if (!c) return out;  // the format ends inside a conversion

    Piece text;
    char prefix[2] = {0, 0};
    int32_t prefix_length = 0;
    bool is_signed = false, negative = false;
    const bool arg_is_wide = (flags & (kLong | kWideSize)) ? true : (flags & kShort) ? false : false;  // settled per type

    switch (c) {
      case 'C':
        flags |= kInvertWide;
        [[fallthrough]];
      case 'c': {
        const bool as_wide = (flags & (kLong | kWideSize)) ? true : (flags & kShort) ? false : (((flags & kInvertWide) != 0) != wide);
        const uint32_t value = args.Next32();
        if (as_wide) {
          wide_char = uint16_t(value);
          text.wide = &wide_char;
          text.wide_is_big_endian = false;
        } else {
          digits[0] = char(uint8_t(value));
          text.narrow = reinterpret_cast<const uint8_t*>(digits);
        }
        text.length = 1;
        break;
      }
      case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'p': {
        const char* alphabet = "0123456789";
        uint32_t radix = 10;
        bool force_leading_zero = false;
        if (c == 'd' || c == 'i') {
          is_signed = true;
        } else if (c == 'o') {
          radix = 8;
          if (flags & kAlternate) force_leading_zero = true;
        } else if (c == 'x' || c == 'X') {
          radix = 16;
          alphabet = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
          if (flags & kAlternate) {
            prefix[0] = '0';
            prefix[1] = char(c);
            prefix_length = 2;
          }
        } else if (c == 'p') {
          radix = 16;
          alphabet = "0123456789ABCDEF";
          precision = 8;
          flags = (flags & ~(kLongLong | kShort)) | kLong;
        }
        int64_t value;
        if (flags & kLongLong) value = int64_t(args.Next64());
        else if (flags & kLong) value = int32_t(args.Next32());
        else if (flags & kShort) value = int16_t(args.Next32());
        else value = int32_t(args.Next32());
        precision = precision >= 0 ? std::min<int32_t>(precision, int32_t(sizeof digits)) : 1;
        if (is_signed && value < 0) {
          value = -value;
          negative = true;
        }
        uint64_t magnitude = uint64_t(value);
        if (!(flags & kLongLong)) magnitude &= 0xFFFFFFFFu;
        if (magnitude == 0) prefix_length = 0;
        char* end = digits + sizeof digits - 1;
        char* start = end;
        *start = 0;
        while (precision-- > 0 || magnitude != 0) {
          *--start = alphabet[magnitude % radix];
          magnitude /= radix;
        }
        if (force_leading_zero && (start == end || *start != '0')) *--start = '0';
        text.narrow = reinterpret_cast<const uint8_t*>(start);
        text.length = int32_t(end - start);
        break;
      }
      case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A': {
        is_signed = true;
        const uint64_t bits = args.Next64();
        double value;
        std::memcpy(&value, &bits, sizeof value);
        if (value < 0) {
          value = -value;
          negative = true;
        }
        const std::string s = FormatDouble(value, precision, c, (flags & kAlternate) != 0);
        const size_t n = std::min(s.size(), sizeof digits - 1);
        std::memcpy(digits, s.data(), n);
        digits[n] = 0;
        text.narrow = reinterpret_cast<const uint8_t*>(digits);
        text.length = int32_t(n);
        break;
      }
      case 'n': {
        const uint32_t pointer = args.Next32();
        if (flags & kShort) g.Store16(pointer, uint16_t(count));
        else g.Store32(pointer, uint32_t(count));
        continue;
      }
      case 'S':
        flags |= kInvertWide;
        [[fallthrough]];
      case 's': {
        const uint32_t pointer = args.Next32();
        const int32_t cap = precision < 0 ? INT32_MAX : precision;
        if (pointer == 0) {
          static const uint8_t kNull[] = "(null)";
          text.narrow = kNull;
          text.length = std::min<int32_t>(6, cap);
        } else {
          const bool as_wide = (flags & (kLong | kWideSize)) ? true : (flags & kShort) ? false : (((flags & kInvertWide) != 0) != wide);
          int32_t length = 0;
          if (as_wide) {
            while (length < cap && g.Load16(pointer + 2 * length)) ++length;
            text.wide = reinterpret_cast<const uint16_t*>(g.host(pointer));
          } else {
            while (length < cap && g.Load8(pointer + length)) ++length;
            text.narrow = g.host(pointer);
          }
          text.length = length;
        }
        break;
      }
      default:
        return out;  // an unknown conversion: invalid format
    }
    (void)arg_is_wide;

    if (is_signed) {
      if (negative) {
        prefix[0] = '-';
        prefix_length = 1;
      } else if (flags & kPlus) {
        prefix[0] = '+';
        prefix_length = 1;
      } else if (flags & kSpace) {
        prefix[0] = ' ';
        prefix_length = 1;
      }
    }
    const int32_t padding = width - text.length - prefix_length;
    if (!(flags & (kLeft | kZeros)) && padding > 0) {
      if (!w.Fill(' ', padding)) return out;
      count += padding;
    }
    for (int32_t i = 0; i < prefix_length; ++i) {
      if (!w.Put(uint8_t(prefix[i]))) return out;
    }
    count += prefix_length;
    if ((flags & kZeros) && !(flags & kLeft) && padding > 0) {
      if (!w.Fill('0', padding)) return out;
      count += padding;
    }
    for (int32_t i = 0; i < text.length; ++i) {
      if (!w.Put(text.At(i))) return out;
    }
    count += text.length;
    if ((flags & kLeft) && padding > 0) {
      if (!w.Fill(' ', padding)) return out;
      count += padding;
    }
  }
}

namespace {

void Store(Guest g, uint32_t buffer, const Formatted& f, int32_t n, bool wide) {
  if (wide) {
    for (int32_t i = 0; i < n; ++i) g.Store16(buffer + 2 * i, uint16_t(f.wide[size_t(i)]));
  } else {
    std::memcpy(g.host(buffer), f.narrow.data(), size_t(n));
  }
}

void Terminate(Guest g, uint32_t buffer, int32_t at, bool wide) {
  if (wide) g.Store16(buffer + 2 * at, 0);
  else g.Store8(buffer + at, 0);
}

}  // namespace

int32_t Sprintf(Guest g, uint32_t buffer, uint32_t format, ArgumentSource& args, bool wide) {
  if (!buffer || !format) return -1;
  const Formatted f = Format(g, format, args, wide);
  if (f.count <= 0) {
    Terminate(g, buffer, 0, wide);
  } else {
    Store(g, buffer, f, f.count, wide);
    Terminate(g, buffer, f.count, wide);
  }
  return f.count;
}

int32_t Snprintf(Guest g, uint32_t buffer, int32_t capacity, uint32_t format, ArgumentSource& args, bool wide,
                 bool count_when_truncated) {
  if (!buffer || capacity <= 0 || !format) return -1;
  const Formatted f = Format(g, format, args, wide);
  if (f.count < 0) {
    Terminate(g, buffer, 0, wide);
    return f.count;
  }
  if (f.count <= capacity) {
    Store(g, buffer, f, f.count, wide);
    if (f.count < capacity) Terminate(g, buffer, f.count, wide);
    return f.count;
  }
  Store(g, buffer, f, capacity, wide);
  return count_when_truncated ? f.count : -1;
}

int32_t Vscwprintf(Guest g, uint32_t format, ArgumentSource& args) {
  if (!format) return -1;
  return Format(g, format, args, true).count;
}

uint32_t DbgPrint(Guest g, uint32_t format, ArgumentSource& args) {
  if (!format) return 0;
  const Formatted f = Format(g, format, args, false);
  if (f.count > 0 && g_debug_sink) {
    size_t n = f.narrow.size();
    while (n && (f.narrow[n - 1] == ' ' || f.narrow[n - 1] == '\n' || f.narrow[n - 1] == '\r' || f.narrow[n - 1] == '\t')) --n;
    g_debug_sink(f.narrow.data(), n);
  }
  return 0;
}

void SetDebugPrintSink(void (*sink)(const char*, size_t)) { g_debug_sink = sink; }

void DebugOutput(Guest g, uint32_t string, bool wide) {
  if (!string || !g_debug_sink) return;
  std::string text;
  if (wide) {
    for (uint32_t at = string; uint16_t c = g.Load16(at); at += 2) text += c < 0x80 ? char(c) : '?';
  } else {
    text = reinterpret_cast<const char*>(g.host(string));
  }
  g_debug_sink(text.data(), text.size());
}

}  // namespace any360::sys
