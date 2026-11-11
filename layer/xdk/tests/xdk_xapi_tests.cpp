// Unit tests of the XAPI replacements (any360/xdk/xapi.h) against their reference models (xapi_reference.h), which
// follow the functions' own steps. Every accepted call must leave memory exactly as the model does, every declined
// call must leave it untouched, and a call is declined only for the reasons the header gives.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "any360/xdk/xapi.h"
#include "any360/xdk/xapi_reference.h"
#include "xdk_test.h"

namespace {

using any360::xdk::Memory;
using xdk_test::kStack;
using xdk_test::Pair;
namespace xdk = any360::xdk;

// One XMemCpy on both memories. Returns whether the replacement accepted it.
bool Copy(Pair& p, uint32_t dest, uint32_t source, uint32_t size, bool small_sizes = true) {
  const bool accepted = xdk::XMemCpy(p.a, dest, source, size, small_sizes);
  if (accepted) xdk::reference::XMemCpy(p.b, dest, source, size);
  return accepted;
}

void Predicates() {
  // Apart and plain; below 16 bytes only when the caller has checked the title's byte step.
  CHECK(xdk::XMemCpyAccepts(0x1000, 0x2000, 0x1000, false));
  CHECK(xdk::XMemCpyAccepts(0x2000, 0x1000, 0x1000, false));
  CHECK(!xdk::XMemCpyAccepts(0x1000, 0x2000, 0x1001, false) && !xdk::XMemCpyAccepts(0x2000, 0x1000, 0x1001, false));
  CHECK(!xdk::XMemCpyAccepts(0x1000, 0x1000, 16, false));
  CHECK(!xdk::XMemCpyAccepts(0x1000, 0x2000, 15, false) && xdk::XMemCpyAccepts(0x1000, 0x2000, 15, true));
  CHECK(xdk::XMemCpyAccepts(0x1000, 0x1000, 0, true) && !xdk::XMemCpyAccepts(0x1000, 0x1000, 0, false));
  CHECK(!xdk::XMemCpyAccepts(0x7F000000, 0x1000, 64, true) && !xdk::XMemCpyAccepts(0x1000, 0x7FC80000, 64, true));
  CHECK(xdk::XMemCpyAccepts(0xE0001000, 0x80001000, 0x1000, true) && !xdk::XMemCpyAccepts(0xDFFFFFF0, 0x1000, 0x20, true));
}

// Ranges apart: every size up to 300 and some long ones, the destination at every offset of a 128-byte cache line
// and a little more, the source at every alignment. Accepted, and as the model's steps leave memory: the bytes
// copied and nothing outside them, whatever the model zeroed ahead.
void Apart() {
  Pair p(1);
  std::vector<uint32_t> sizes;
  for (uint32_t size = 0; size <= 300; ++size) sizes.push_back(size);
  for (uint32_t size : {383u, 384u, 385u, 511u, 512u, 513u, 1023u, 1024u, 1025u, 1300u, 2047u, 4096u, 5000u}) sizes.push_back(size);
  for (uint32_t size : sizes) {
    for (uint32_t dest_offset = 0; dest_offset < 144; dest_offset += size > 300 ? 7 : 1) {
      for (uint32_t source_align = 0; source_align < 16; source_align += size > 300 ? 3 : 1) {
        const uint32_t dest = 0x10000 + dest_offset, source = 0x40000 + source_align;
        CHECK(Copy(p, dest, source, size));
        CHECK(p.Same(dest, dest + size, 300));
      }
    }
    CHECK(p.SameEverywhere());
  }
  // The destination above the source, and the two ranges touching.
  for (uint32_t size : {16u, 17u, 100u, 128u, 129u, 256u, 257u, 1000u}) {
    CHECK(Copy(p, 0x60000 + size, 0x60000, size) && Copy(p, 0x70000, 0x70000 + size, size));
    CHECK(Copy(p, 0x80000 + 3, 0x50000 + 5, size));
  }
  CHECK(p.SameEverywhere());
}

// Ranges that overlap: declined, nothing written. And why: by the model's steps such a call leaves what neither
// memmove nor a forward copy leaves (the cache line zeroed ahead held source bytes not read yet).
void Overlapping() {
  Pair p(2);
  for (uint32_t size = 1; size <= 300; ++size) {
    for (uint32_t distance = 0; distance < size; distance += size > 64 ? 5 : 1) {
      CHECK(!Copy(p, 0x20000 + distance, 0x20000, size));
      CHECK(!Copy(p, 0x20000, 0x20000 + distance, size));
    }
  }
  CHECK(p.SameEverywhere());
  const uint32_t dest = 0x30000, source = dest + 64, size = 1024;
  std::vector<uint8_t> moved(p.model.begin() + source, p.model.begin() + source + size);  // what memmove would leave
  xdk::reference::XMemCpy(p.b, dest, source, size);
  CHECK(std::memcmp(&p.model[dest], moved.data(), size) != 0);
  bool zeroes = true;
  for (uint32_t i = 0; i < 64; ++i) zeroes = zeroes && p.model[dest + i] == 0;  // the zeroed source bytes, copied
  CHECK(zeroes);
}

// Below 16 bytes without the title's byte step checked, and memory that is not plain: declined.
void Declined() {
  Pair p(3);
  for (uint32_t size = 0; size < 16; ++size) CHECK(!Copy(p, 0x1000, 0x2000, size, false));
  CHECK(Copy(p, 0x1000, 0x2000, 16, false));
  CHECK(p.SameEverywhere());
  const Memory nowhere{};
  for (uint32_t address : {0x7F000000u, 0x7FC80000u, 0x7EFFFFF8u, 0xDFFFFFF8u, 0xFFFFFFF8u}) {
    CHECK(!xdk::XMemCpy(nowhere, address, 0x1000, 64, true));
    CHECK(!xdk::XMemCpy(nowhere, 0x1000, address, 64, true));
  }
}

// Calls at random; accepted exactly when the header's test says.
void Random(uint32_t seed, uint32_t count) {
  Pair p(seed + 10);
  std::mt19937 rng(seed * 104729u);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t kind = rng() % 8;
    const uint32_t size = kind < 3 ? rng() % 64 : kind < 6 ? rng() % 700 : rng() % 20000;
    const uint32_t limit = kStack - 512 - size;
    const uint32_t dest = 512 + rng() % (limit - 512);
    uint32_t source = 512 + rng() % (limit - 512);
    if (rng() % 4 == 0) {
      const int64_t near = int64_t(dest) + int32_t(rng() % 1025) - 512;
      if (near >= 512 && near < limit) source = uint32_t(near);
    }
    const bool small_sizes = rng() & 1;
    const bool accepted = Copy(p, dest, source, size, small_sizes);
    CHECK(accepted == xdk::XMemCpyAccepts(dest, source, size, small_sizes));
    const bool apart = dest >= source ? dest - source >= size : source - dest >= size;
    CHECK(accepted == (apart && (size >= 16 || small_sizes)));
    CHECK(p.Same(dest, dest + size, 300));
    if (i % 2048 == 0) CHECK(p.SameEverywhere());
  }
  CHECK(p.SameEverywhere());
}

}  // namespace

// GetLastError and SetLastError on a control region and a thread made up in the arena: the field read and written,
// nothing while a DPC runs, the calls declined when either is not plain memory.
void LastError() {
  Pair p(91);
  std::mt19937 rng(92);
  const auto put = [&](uint32_t at, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p.native[at + i] = p.model[at + i] = uint8_t(value >> (24 - 8 * i));
  };
  uint64_t r3 = 0;
  for (uint32_t round = 0; round < 2000; ++round) {
    const uint32_t pcr = 0x4000 + (rng() % 0x400) * 4, thread = 0x20000 + (rng() % 0x1000) * 4;
    const uint32_t dpc = round % 5 == 0 ? 1 + rng() % 7 : 0, error = rng();
    put(pcr + 336, dpc);
    put(pcr + 256, thread);
    put(thread + 352, rng());
    xdk::Range write;
    CHECK(xdk::XapiSetLastErrorPlan(p.a, pcr, write) && (dpc ? write.size == 0 : write.at == thread + 352 && write.size == 4));
    CHECK(xdk::XapiSetLastError(p.a, pcr, error));
    xdk::reference::SetLastError(p.b, pcr, error);
    CHECK(p.Same(thread + 352, thread + 356));
    CHECK(xdk::XapiGetLastError(p.a, pcr, r3) && r3 == xdk::reference::GetLastError(p.b, pcr) && r3 == (dpc ? 0u : error));
  }
  CHECK(p.SameEverywhere());
  // A thread in the GPU's register window, a control region that reaches it: declined, nothing written.
  put(0x4000 + 336, 0);
  put(0x4000 + 256, 0x7F000000);
  CHECK(!xdk::XapiGetLastError(p.a, 0x4000, r3) && !xdk::XapiSetLastError(p.a, 0x4000, 5) && p.SameEverywhere());
  const Memory nowhere{};
  CHECK(!xdk::XapiGetLastError(nowhere, 0x7EFFFF00, r3) && !xdk::XapiSetLastError(nowhere, 0x7EFFFF00, 5));
}

int main() {
  Predicates();
  LastError();
  Apart();
  Overlapping();
  Declined();
  for (uint32_t seed = 1; seed <= 3; ++seed) Random(seed, 40000);
  return xdk_test::Finish("xdk_xapi_tests");
}
