#include "any360/sys/png.h"

#include <array>
#include <cstring>

namespace any360::sys {

namespace {

bool Fail(std::string* error, const char* what) {
  if (error) *error = what;
  return false;
}

// Bits read least significant first, as deflate packs them.
struct Bits {
  const uint8_t* data;
  size_t size, pos = 0;  // pos: the next byte to load
  uint64_t buffer = 0;
  uint32_t count = 0;  // bits in buffer
  bool overrun = false;

  void Fill() {
    while (count <= 56) {
      if (pos < size) {
        buffer |= uint64_t(data[pos++]) << count;
      } else if (pos < size + 8) {
        ++pos;  // zeros past the end; reading them is an overrun, checked when consumed
      } else {
        break;
      }
      count += 8;
    }
  }
  uint32_t Peek(uint32_t n) {
    if (count < n) Fill();
    return uint32_t(buffer & ((uint64_t(1) << n) - 1));
  }
  void Drop(uint32_t n) {
    if (n > count) {
      overrun = true;
      buffer = 0;
      count = 0;
      return;
    }
    buffer >>= n;
    count -= n;
    // Bits consumed past the input's end: those of the zero bytes Fill appended.
    if (pos * 8 - count > size * 8) overrun = true;
  }
  uint32_t Take(uint32_t n) {
    if (n == 0) return 0;
    const uint32_t v = Peek(n);
    Drop(n);
    return v;
  }
  void AlignToByte() { Drop(count & 7); }
};

constexpr uint32_t kFastBits = 9;

// A canonical Huffman code (RFC 1951, 3.2.2): codes up to kFastBits long by a table of the next bits, longer ones
// bit by bit (zlib's puff).
struct Huffman {
  std::array<uint16_t, 16> count{};
  std::array<uint16_t, 320> symbol{};
  std::array<uint16_t, 1u << kFastBits> fast{};  // (length << 9) | symbol, 0: not a short code

  // False when the lengths over-subscribe the code space.
  bool Build(const uint8_t* lengths, uint32_t n) {
    count.fill(0);
    fast.fill(0);
    for (uint32_t s = 0; s < n; ++s) ++count[lengths[s]];
    count[0] = 0;
    int left = 1;
    for (uint32_t len = 1; len < 16; ++len) {
      left = (left << 1) - count[len];
      if (left < 0) return false;
    }
    std::array<uint16_t, 16> offset{};
    for (uint32_t len = 1; len < 15; ++len) offset[len + 1] = uint16_t(offset[len] + count[len]);
    for (uint32_t s = 0; s < n; ++s)
      if (lengths[s]) symbol[offset[lengths[s]]++] = uint16_t(s);
    // The short codes' table: canonical codes, bit-reversed as they arrive.
    uint32_t code = 0, index = 0;
    for (uint32_t len = 1; len <= kFastBits; ++len) {
      for (uint32_t i = 0; i < count[len]; ++i, ++code, ++index) {
        uint32_t reversed = 0;
        for (uint32_t b = 0; b < len; ++b) reversed |= ((code >> b) & 1) << (len - 1 - b);
        for (uint32_t fill = reversed; fill < (1u << kFastBits); fill += 1u << len)
          fast[fill] = uint16_t(len << 9 | symbol[index]);
      }
      code <<= 1;
    }
    return true;
  }

  // The next symbol, or -1 for a code the lengths left unused.
  int Decode(Bits& bits) const {
    const uint16_t f = fast[bits.Peek(kFastBits)];
    if (f) {
      bits.Drop(f >> 9);
      return f & 0x1FF;
    }
    int code = 0, first = 0, index = 0;
    for (uint32_t len = 1; len < 16; ++len) {
      code |= int(bits.Take(1));
      const int n = count[len];
      if (code - n < first) return symbol[index + (code - first)];
      index += n;
      first += n;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& bits, const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out, size_t start, std::string* error) {
  for (;;) {
    int s = lit.Decode(bits);
    if (bits.overrun) return Fail(error, "deflate: input ends inside a block");
    if (s < 0) return Fail(error, "deflate: unused literal/length code");
    if (s < 256) {
      out.push_back(uint8_t(s));
    } else if (s == 256) {
      return true;
    } else {
      s -= 257;
      if (s >= 29) return Fail(error, "deflate: invalid length symbol");
      const uint32_t length = kLengthBase[s] + bits.Take(kLengthExtra[s]);
      const int d = dist.Decode(bits);
      if (d < 0 || d >= 30) return Fail(error, "deflate: invalid distance symbol");
      const uint32_t distance = kDistBase[d] + bits.Take(kDistExtra[d]);
      if (bits.overrun) return Fail(error, "deflate: input ends inside a block");
      if (distance > out.size() - start) return Fail(error, "deflate: distance before the start of the output");
      size_t from = out.size() - distance;
      out.resize(out.size() + length);
      uint8_t* p = out.data();
      size_t to = out.size() - length;
      if (distance >= length) {
        std::memcpy(p + to, p + from, length);
      } else {
        for (uint32_t i = 0; i < length; ++i) p[to + i] = p[from + i];  // overlapping: repeats what it writes
      }
    }
  }
}

const Huffman& FixedLit() {
  static const Huffman h = [] {
    uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    Huffman f;
    f.Build(lengths, 288);
    return f;
  }();
  return h;
}

const Huffman& FixedDist() {
  static const Huffman h = [] {
    uint8_t lengths[30];
    std::memset(lengths, 5, sizeof(lengths));
    Huffman f;
    f.Build(lengths, 30);
    return f;
  }();
  return h;
}

bool Dynamic(Bits& bits, Huffman& lit, Huffman& dist, std::string* error) {
  const uint32_t nlen = bits.Take(5) + 257, ndist = bits.Take(5) + 1, ncode = bits.Take(4) + 4;
  if (nlen > 286 || ndist > 30) return Fail(error, "deflate: too many codes");
  static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint8_t lengths[320] = {};
  for (uint32_t i = 0; i < ncode; ++i) lengths[kOrder[i]] = uint8_t(bits.Take(3));
  Huffman lencode;
  if (!lencode.Build(lengths, 19)) return Fail(error, "deflate: over-subscribed code length code");
  uint32_t index = 0;
  while (index < nlen + ndist) {
    const int s = lencode.Decode(bits);
    if (bits.overrun) return Fail(error, "deflate: input ends in the code lengths");
    if (s < 0) return Fail(error, "deflate: unused code length code");
    if (s < 16) {
      lengths[index++] = uint8_t(s);
      continue;
    }
    uint8_t value = 0;
    uint32_t repeat;
    if (s == 16) {
      if (index == 0) return Fail(error, "deflate: repeat with no previous length");
      value = lengths[index - 1];
      repeat = 3 + bits.Take(2);
    } else if (s == 17) {
      repeat = 3 + bits.Take(3);
    } else {
      repeat = 11 + bits.Take(7);
    }
    if (index + repeat > nlen + ndist) return Fail(error, "deflate: too many code lengths");
    while (repeat--) lengths[index++] = value;
  }
  if (lengths[256] == 0) return Fail(error, "deflate: no end-of-block code");
  if (!lit.Build(lengths, nlen)) return Fail(error, "deflate: over-subscribed literal/length code");
  if (!dist.Build(lengths + nlen, ndist)) return Fail(error, "deflate: over-subscribed distance code");
  return true;
}

bool InflateBits(Bits& bits, std::vector<uint8_t>& out, std::string* error) {
  const size_t start = out.size();
  Huffman lit, dist;
  for (;;) {
    const uint32_t last = bits.Take(1), type = bits.Take(2);
    if (bits.overrun) return Fail(error, "deflate: input ends before a block");
    if (type == 0) {
      bits.AlignToByte();
      const uint32_t len = bits.Take(16), nlen = bits.Take(16);
      if (bits.overrun) return Fail(error, "deflate: input ends in a stored block's header");
      if ((len ^ 0xFFFF) != nlen) return Fail(error, "deflate: stored block length check");
      for (uint32_t i = 0; i < len; ++i) out.push_back(uint8_t(bits.Take(8)));
      if (bits.overrun) return Fail(error, "deflate: input ends in a stored block");
    } else if (type == 1) {
      if (!Codes(bits, FixedLit(), FixedDist(), out, start, error)) return false;
    } else if (type == 2) {
      if (!Dynamic(bits, lit, dist, error) || !Codes(bits, lit, dist, out, start, error)) return false;
    } else {
      return Fail(error, "deflate: invalid block type");
    }
    if (last) return true;
  }
}

uint32_t Adler32(const uint8_t* p, size_t n) {
  uint32_t a = 1, b = 0;
  while (n) {
    const size_t chunk = n < 5552 ? n : 5552;  // the most bytes before b can overflow
    for (size_t i = 0; i < chunk; ++i) {
      a += p[i];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    p += chunk;
    n -= chunk;
  }
  return b << 16 | a;
}

uint32_t Crc32(const uint8_t* p, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint8_t Paeth(int a, int b, int c) {
  const int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

bool Inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::string* error) {
  Bits bits{in.data(), in.size()};
  return InflateBits(bits, out, error);
}

bool ZlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, std::string* error) {
  if (in.size() < 6) return Fail(error, "zlib: too short");
  if ((in[0] & 0x0F) != 8 || (in[0] >> 4) > 7) return Fail(error, "zlib: not deflate");
  if ((uint32_t(in[0]) << 8 | in[1]) % 31) return Fail(error, "zlib: header check");
  if (in[1] & 0x20) return Fail(error, "zlib: preset dictionary");
  const size_t start = out.size();
  Bits bits{in.data() + 2, in.size() - 2};
  if (!InflateBits(bits, out, error)) return false;
  bits.AlignToByte();
  // The Adler-32 follows the last block's byte: the bytes the bit buffer read ahead are given back.
  const size_t used = 2 + bits.pos - bits.count / 8;
  if (used + 4 > in.size()) return Fail(error, "zlib: no Adler-32");
  if (Be32(in.data() + used) != Adler32(out.data() + start, out.size() - start)) return Fail(error, "zlib: Adler-32 check");
  return true;
}

bool DecodePng(std::span<const uint8_t> file, RgbaImage& out, std::string* error) {
  static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0) return Fail(error, "png: not a PNG file");
  uint32_t width = 0, height = 0, color = 0;
  bool header = false, end = false;
  std::vector<uint8_t> idat;
  std::array<uint32_t, 256> palette{};
  uint32_t palette_size = 0;
  for (size_t at = 8; !end;) {
    if (at + 12 > file.size()) return Fail(error, "png: truncated chunk");
    const uint32_t length = Be32(file.data() + at);
    if (length > file.size() - at - 12) return Fail(error, "png: truncated chunk");
    const uint8_t* type = file.data() + at + 4;
    const uint8_t* data = type + 4;
    if (Be32(data + length) != Crc32(type, length + 4)) return Fail(error, "png: chunk CRC check");
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (length != 13) return Fail(error, "png: bad IHDR");
      width = Be32(data);
      height = Be32(data + 4);
      const uint32_t depth = data[8];
      color = data[9];
      if (width == 0 || height == 0 || width > 16384 || height > 16384) return Fail(error, "png: unsupported size");
      if (depth != 8) return Fail(error, "png: only 8-bit samples are handled");
      if (color != 0 && color != 2 && color != 3 && color != 4 && color != 6) return Fail(error, "png: bad colour type");
      if (data[10] != 0 || data[11] != 0) return Fail(error, "png: unknown compression or filter method");
      if (data[12] != 0) return Fail(error, "png: interlaced images are not handled");
      header = true;
    } else if (!header) {
      return Fail(error, "png: IHDR is not first");
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      if (length % 3 || length > 768) return Fail(error, "png: bad PLTE");
      palette_size = length / 3;
      for (uint32_t i = 0; i < palette_size; ++i)
        palette[i] = data[3 * i] | uint32_t(data[3 * i + 1]) << 8 | uint32_t(data[3 * i + 2]) << 16 | 0xFF000000u;
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      if (color == 3)
        for (uint32_t i = 0; i < length && i < 256; ++i) palette[i] = (palette[i] & 0x00FFFFFFu) | uint32_t(data[i]) << 24;
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      idat.insert(idat.end(), data, data + length);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      end = true;
    } else if (!(type[0] & 0x20)) {
      return Fail(error, "png: unknown critical chunk");
    }
    at += size_t(length) + 12;
  }
  if (color == 3 && palette_size == 0) return Fail(error, "png: palette image without PLTE");
  const uint32_t channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : 4;
  const size_t stride = size_t(width) * channels;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * height);
  if (!ZlibInflate(idat, raw, error)) return false;
  if (raw.size() != (stride + 1) * height) return Fail(error, "png: image data size");

  // Rows unfiltered in place (each one's filter byte dropped by reading past it).
  const uint32_t bpp = channels;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = raw.data() + y * (stride + 1);
    const uint8_t filter = row[0];
    uint8_t* cur = row + 1;
    const uint8_t* prev = y ? raw.data() + (y - 1) * (stride + 1) + 1 : nullptr;
    switch (filter) {
      case 0: break;
      case 1:
        for (size_t i = bpp; i < stride; ++i) cur[i] = uint8_t(cur[i] + cur[i - bpp]);
        break;
      case 2:
        if (prev)
          for (size_t i = 0; i < stride; ++i) cur[i] = uint8_t(cur[i] + prev[i]);
        break;
      case 3:
        for (size_t i = 0; i < stride; ++i) {
          const int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0;
          cur[i] = uint8_t(cur[i] + ((a + b) >> 1));
        }
        break;
      case 4:
        for (size_t i = 0; i < stride; ++i) {
          const int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
          cur[i] = uint8_t(cur[i] + Paeth(a, b, c));
        }
        break;
      default: return Fail(error, "png: bad filter type");
    }
  }

  out.width = width;
  out.height = height;
  out.pixels.resize(size_t(width) * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* s = raw.data() + y * (stride + 1) + 1;
    uint32_t* d = out.pixels.data() + size_t(y) * width;
    for (uint32_t x = 0; x < width; ++x) {
      switch (color) {
        case 0: d[x] = s[x] * 0x010101u | 0xFF000000u; break;
        case 2: d[x] = s[3 * x] | uint32_t(s[3 * x + 1]) << 8 | uint32_t(s[3 * x + 2]) << 16 | 0xFF000000u; break;
        case 3:
          if (s[x] >= palette_size) return Fail(error, "png: palette index out of range");
          d[x] = palette[s[x]];
          break;
        case 4: d[x] = s[2 * x] * 0x010101u | uint32_t(s[2 * x + 1]) << 24; break;
        default: std::memcpy(&d[x], s + 4 * x, 4); break;  // RGBA: R in the low byte on a little-endian host
      }
    }
  }
  return true;
}

}  // namespace any360::sys
