// Deflate streams for the tests and the benchmark of zlib 1.1.4's inflate_fast (any360/xdk/zlib.h): tables built
// as zlib 1.1.4's huft_build builds them (8-byte entries, the same operation codes), random complete Huffman
// codes and the fixed ones, streams of random symbols coded with them, and the guest layout of a call.
#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace xdk_zlib {

// Guest layout of a call.
constexpr uint32_t kMasks = 0x1000, kMsgLiteral = 0x1100, kMsgDistance = 0x1120, kZ = 0x2000, kS = 0x2100;
constexpr uint32_t kTables = 0x10000, kInput = 0x30000, kWindow = 0x60000;

struct Entry {
  uint8_t exop = 0, bits = 0;
  uint32_t base = 0;
};

// zlib 1.1.4's huft_build (inftrees.c), on host memory. Returns 0 (Z_OK), -5 (Z_BUF_ERROR: incomplete) or -3.
inline int HuftBuild(const uint32_t* b, uint32_t n, uint32_t s, const uint32_t* d, const uint32_t* e, uint32_t& t, uint32_t& m,
              std::vector<Entry>& hp) {
  constexpr int BMAX = 15;
  uint32_t c[BMAX + 1] = {}, x[BMAX + 1] = {}, v[288] = {};
  uint32_t u[BMAX] = {};
  for (uint32_t i = 0; i < n; ++i) c[b[i]]++;
  if (c[0] == n) {
    t = 0, m = 0;
    return 0;
  }
  int l = int(m);
  uint32_t j, i;
  for (j = 1; j <= uint32_t(BMAX); j++)
    if (c[j]) break;
  int k = int(j);
  if (uint32_t(l) < j) l = int(j);
  for (i = BMAX; i; i--)
    if (c[i]) break;
  const int g = int(i);
  if (uint32_t(l) > i) l = int(i);
  m = uint32_t(l);
  int y;
  for (y = 1 << j; j < i; j++, y <<= 1)
    if ((y -= int(c[j])) < 0) return -3;
  if ((y -= int(c[i])) < 0) return -3;
  c[i] += uint32_t(y);
  x[1] = j = 0;
  {
    uint32_t* p = c + 1;
    uint32_t* xp = x + 2;
    while (--i) *xp++ = (j += *p++);
  }
  for (i = 0; i < n; ++i)
    if ((j = b[i]) != 0) v[x[j]++] = i;
  n = x[g];
  x[0] = i = 0;
  uint32_t pv = 0;  // index into v
  int h = -1, w = -l;
  uint32_t q = 0, z = 0, f = 0;
  Entry r;
  for (; k <= g; k++) {
    uint32_t a = c[k];
    while (a--) {
      while (k > w + l) {
        h++;
        w += l;
        z = uint32_t(g - w);
        z = z > uint32_t(l) ? uint32_t(l) : z;
        if ((f = 1u << (j = uint32_t(k - w))) > a + 1) {
          f -= a + 1;
          uint32_t xp = uint32_t(k);
          if (j < z)
            while (++j < z) {
              if ((f <<= 1) <= c[++xp]) break;
              f -= c[xp];
            }
        }
        z = 1u << j;
        if (hp.size() + z > 1440) return -3;
        u[h] = q = uint32_t(hp.size());
        hp.resize(hp.size() + z);
        if (h) {
          x[h] = i;
          r.bits = uint8_t(l);
          r.exop = uint8_t(j);
          j = i >> (w - l);
          r.base = q - u[h - 1] - j;
          hp[u[h - 1] + j] = r;
        } else {
          t = q;
        }
      }
      r.bits = uint8_t(k - w);
      if (pv >= n) {
        r.exop = 128 + 64;
      } else if (v[pv] < s) {
        r.exop = uint8_t(v[pv] < 256 ? 0 : 32 + 64);
        r.base = v[pv++];
      } else {
        r.exop = uint8_t(e[v[pv] - s] + 16 + 64);
        r.base = d[v[pv++] - s];
      }
      f = 1u << (k - w);
      for (j = i >> w; j < z; j += f) hp[q + j] = r;
      for (j = 1u << (k - 1); i & j; j >>= 1) i ^= j;
      i ^= j;
      uint32_t mask = (1u << w) - 1;
      while ((i & mask) != x[h]) {
        h--;
        w -= l;
        mask = (1u << w) - 1;
      }
    }
  }
  return y != 0 && g != 1 ? -5 : 0;
}

inline const uint32_t kLens[31] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
inline const uint32_t kLenExtra[31] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 112, 112};
inline const uint32_t kDists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline const uint32_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Huffman code lengths (at most 15) of random frequencies, every symbol used.
inline std::vector<uint32_t> RandomLengths(uint32_t symbols, std::mt19937& rng) {
  for (;;) {
    std::vector<uint64_t> weight(symbols);
    const uint32_t spread = 1 + rng() % 12;
    for (auto& w : weight) w = 1 + (rng() >> (32 - spread));
    // Huffman by repeated merging (n is small).
    std::vector<int> parent(2 * symbols, -1);
    std::vector<std::pair<uint64_t, int>> heap;
    for (uint32_t i = 0; i < symbols; ++i) heap.push_back({weight[i], int(i)});
    int next = int(symbols);
    auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };
    std::make_heap(heap.begin(), heap.end(), cmp);
    while (heap.size() > 1) {
      std::pop_heap(heap.begin(), heap.end(), cmp);
      const auto a = heap.back();
      heap.pop_back();
      std::pop_heap(heap.begin(), heap.end(), cmp);
      const auto b = heap.back();
      heap.pop_back();
      parent[a.second] = parent[b.second] = next;
      heap.push_back({a.first + b.first, next++});
      std::push_heap(heap.begin(), heap.end(), cmp);
    }
    std::vector<uint32_t> lengths(symbols);
    bool fits = true;
    for (uint32_t i = 0; i < symbols; ++i) {
      uint32_t depth = 0;
      for (int p = int(i); parent[p] >= 0; p = parent[p]) ++depth;
      lengths[i] = std::max<uint32_t>(depth, 1);
      fits = fits && depth <= 15;
    }
    if (fits) return lengths;
  }
}

// deflate's canonical codes, bit-reversed for the stream (its first bit is the code's top bit).
inline std::vector<uint32_t> Codes(const std::vector<uint32_t>& lengths) {
  uint32_t count[16] = {}, next[16] = {};
  for (uint32_t l : lengths) count[l]++;
  count[0] = 0;
  for (uint32_t bits = 1, code = 0; bits <= 15; ++bits) next[bits] = code = (code + count[bits - 1]) << 1;
  std::vector<uint32_t> codes(lengths.size());
  for (size_t i = 0; i < lengths.size(); ++i) {
    if (!lengths[i]) continue;
    uint32_t c = next[lengths[i]]++, r = 0;
    for (uint32_t b = 0; b < lengths[i]; ++b) r = (r << 1) | ((c >> b) & 1);
    codes[i] = r;
  }
  return codes;
}

struct BitWriter {
  std::vector<uint8_t> bytes;
  uint64_t acc = 0;
  uint32_t count = 0;
  void Put(uint32_t value, uint32_t bits) {
    acc |= uint64_t(value) << count;
    count += bits;
    while (count >= 8) bytes.push_back(uint8_t(acc)), acc >>= 8, count -= 8;
  }
  void Flush() {
    if (count) bytes.push_back(uint8_t(acc));
    acc = 0, count = 0;
  }
};

struct Case {
  std::vector<Entry> tables;
  uint32_t tl = 0, td = 0, bl = 0, bd = 0;
  std::vector<uint8_t> input;  // after the bits already in the buffer
  uint32_t b = 0, k = 0;
  uint32_t window_size = 0x8000, write = 0, read = 0;
};

// A block's tables and a stream of random symbols for them.
// `literal_share`: the percentage of literals among the symbols (random when above 100).
inline Case MakeCase(std::mt19937& rng, bool fixed, uint32_t symbols, uint32_t max_distance, uint32_t literal_share = 101) {
  Case c;
  std::vector<uint32_t> ll, dl;
  if (fixed) {
    ll.assign(288, 8);
    for (uint32_t i = 144; i < 256; ++i) ll[i] = 9;
    for (uint32_t i = 256; i < 280; ++i) ll[i] = 7;
    dl.assign(30, 5);
    c.bl = 9, c.bd = 5;
  } else {
    ll = RandomLengths(286, rng);
    dl = RandomLengths(30, rng);
    c.bl = 9, c.bd = 6;
  }
  HuftBuild(ll.data(), uint32_t(ll.size()), 257, kLens, kLenExtra, c.tl, c.bl, c.tables);
  HuftBuild(dl.data(), uint32_t(dl.size()), 0, kDists, kDistExtra, c.td, c.bd, c.tables);
  const auto lcodes = Codes(ll), dcodes = Codes(dl);
  BitWriter w;
  // Bits of an earlier symbol, in the first byte: the call starts with them in its bit buffer.
  const uint32_t skip = rng() % 8;
  w.Put(rng() & ((1u << skip) - 1), skip);
  if (literal_share > 100) literal_share = rng() % 100;
  for (uint32_t i = 0; i < symbols; ++i) {
    if (rng() % 100 < literal_share) {
      const uint32_t lit = rng() % 256;
      w.Put(lcodes[lit], ll[lit]);
      continue;
    }
    const uint32_t ls = rng() % 29, length = kLens[ls] + (kLenExtra[ls] ? rng() % (1u << kLenExtra[ls]) : 0);
    (void)length;
    w.Put(lcodes[257 + ls], ll[257 + ls]);
    if (kLenExtra[ls]) w.Put(rng() & ((1u << kLenExtra[ls]) - 1), kLenExtra[ls]);
    uint32_t ds;
    do ds = rng() % 30;
    while (kDists[ds] > max_distance);
    w.Put(dcodes[ds], dl[ds]);
    if (kDistExtra[ds]) w.Put(rng() & ((1u << kDistExtra[ds]) - 1), kDistExtra[ds]);
  }
  if (rng() % 2) w.Put(lcodes[256], ll[256]);  // the end of the block
  for (int i = 0; i < 8; ++i) w.Put(rng() & 0xFF, 8);  // what follows the block
  w.Flush();
  c.k = 8 - skip;
  c.b = w.bytes[0] >> skip;
  c.input.assign(w.bytes.begin() + 1, w.bytes.end());
  if (rng() % 3 == 0 && c.input.size() > 12) {  // one more whole byte already in the buffer
    c.b |= uint32_t(c.input[0]) << c.k;
    c.k += 8;
    c.input.erase(c.input.begin());
  }
  return c;
}

inline void Store32(std::vector<uint8_t>& mem, uint32_t at, uint32_t v) {
  mem[at] = uint8_t(v >> 24), mem[at + 1] = uint8_t(v >> 16), mem[at + 2] = uint8_t(v >> 8), mem[at + 3] = uint8_t(v);
}

inline void Place(std::vector<uint8_t>& mem, const Case& c) {
  for (uint32_t i = 0; i < 17; ++i) Store32(mem, kMasks + 4 * i, (1u << i) - 1);
  for (size_t i = 0; i < c.tables.size(); ++i) {
    const uint32_t at = kTables + uint32_t(i) * 8;
    mem[at] = c.tables[i].exop, mem[at + 1] = c.tables[i].bits;
    Store32(mem, at + 4, c.tables[i].base);
  }
  std::copy(c.input.begin(), c.input.end(), mem.begin() + kInput);
  Store32(mem, kZ + 0, kInput);
  Store32(mem, kZ + 4, uint32_t(c.input.size()));
  Store32(mem, kZ + 8, 1234567);
  Store32(mem, kS + 28, c.k);
  Store32(mem, kS + 32, c.b);
  Store32(mem, kS + 40, kWindow);
  Store32(mem, kS + 44, kWindow + c.window_size);
  Store32(mem, kS + 48, kWindow + c.read);
  Store32(mem, kS + 52, kWindow + c.write);
}

}  // namespace xdk_zlib
