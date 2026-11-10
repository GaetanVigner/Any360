// Unit tests of the C runtime replacements (any360/xdk/crt.h) against their reference models (crt_reference.h):
// every accepted call must leave memory exactly as the model does, every declined call must leave it untouched, and
// a call is declined only for the reasons the header gives (so a replacement cannot pass by declining). Sizes and
// alignments exhaustively where they are small, then at random; the ranges that are not plain memory; the physical
// view's host offset.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "any360/xdk/crt.h"
#include "any360/xdk/crt_reference.h"

namespace {

using any360::xdk::Memory;
namespace xdk = any360::xdk;

int g_failures = 0;
uint64_t g_checks = 0;
#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    if (!(condition)) {                                                         \
      if (++g_failures <= 20) std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                           \
  } while (0)

// Two copies of the same guest memory: the replacement works on one, the model on the other.
constexpr uint32_t kArena = 1u << 20;       // guest addresses [0, kArena)
constexpr uint32_t kStack = kArena - 256;   // the stack pointer of the calls: its slot sits at kStack - 8
struct Pair {
  std::vector<uint8_t> native, model;
  Memory a, b;
  explicit Pair(uint32_t seed) : native(kArena), model(kArena) {
    std::mt19937 rng(seed);
    for (uint32_t i = 0; i < kArena; i += 4) {
      const uint32_t v = rng();
      std::memcpy(&native[i], &v, 4);
    }
    model = native;
    a.base = native.data();
    b.base = model.data();
  }
  // The two memories agree over [begin, end) and at the stack slot.
  bool Same(uint32_t begin, uint32_t end) const {
    begin = begin > 64 ? begin - 64 : 0;
    end = end + 64 < kArena ? end + 64 : kArena;
    return std::memcmp(&native[begin], &model[begin], end - begin) == 0 &&
           std::memcmp(&native[kStack - 16], &model[kStack - 16], 32) == 0;
  }
  bool SameEverywhere() const { return native == model; }
};

enum Function { kMemcpy, kCopyForward, kMemset };

// One call on both memories. Returns whether the replacement accepted it. `upper` is the high half of the
// destination's register (memcpy saves the whole register).
bool Call(Pair& p, Function f, uint32_t dest, uint32_t source_or_value, uint32_t size, uint32_t upper = 0) {
  bool accepted = false;
  switch (f) {
    case kMemcpy:
      accepted = xdk::CrtMemcpy(p.a, uint64_t(upper) << 32 | dest, source_or_value, size, kStack);
      if (accepted) xdk::reference::Memcpy(p.b, uint64_t(upper) << 32 | dest, source_or_value, size, kStack);
      break;
    case kCopyForward:
      accepted = xdk::CrtCopyForward(p.a, dest, source_or_value, size);
      if (accepted) xdk::reference::CopyForward(p.b, dest, source_or_value, size);
      break;
    case kMemset:
      accepted = xdk::CrtMemset(p.a, dest, source_or_value, size);
      if (accepted) xdk::reference::Memset(p.b, dest, source_or_value, size);
      break;
  }
  return accepted;
}

void Predicates() {
  // Plain memory: below the GPU's registers, between them and the physical view, inside the physical view.
  CHECK(xdk::IsPlainMemory(0, 0));
  CHECK(xdk::IsPlainMemory(0x1000, 0x1000));
  CHECK(xdk::IsPlainMemory(0x7EFFFFF0, 0x10));
  CHECK(!xdk::IsPlainMemory(0x7EFFFFF0, 0x11));
  CHECK(!xdk::IsPlainMemory(0x7F000000, 1));
  CHECK(!xdk::IsPlainMemory(0x7FC80000, 4));
  CHECK(!xdk::IsPlainMemory(0x7FFFFFFF, 1));
  CHECK(!xdk::IsPlainMemory(0x7FFFFFFF, 2));
  CHECK(xdk::IsPlainMemory(0x80000000, 0x1000));
  CHECK(xdk::IsPlainMemory(0xDFFFFFF0, 0x10));
  CHECK(!xdk::IsPlainMemory(0xDFFFFFF0, 0x11));
  CHECK(xdk::IsPlainMemory(0xE0000000, 0x1000));
  CHECK(xdk::IsPlainMemory(0xFFFFFFF0, 0x10));
  CHECK(!xdk::IsPlainMemory(0xFFFFFFF0, 0x11));
  CHECK(!xdk::IsPlainMemory(0x10, 0xFFFFFFFF));
  // Forward copies that equal memmove: destination at or below the source, or past its end.
  CHECK(xdk::IsPlainForwardCopy(100, 100, 50));
  CHECK(xdk::IsPlainForwardCopy(100, 101, 50));
  CHECK(xdk::IsPlainForwardCopy(100, 200, 50));
  CHECK(xdk::IsPlainForwardCopy(150, 100, 50));
  CHECK(!xdk::IsPlainForwardCopy(149, 100, 50));
  CHECK(!xdk::IsPlainForwardCopy(101, 100, 50));
  CHECK(xdk::IsPlainForwardCopy(101, 100, 1));
  CHECK(xdk::IsPlainForwardCopy(101, 100, 0));
}

// Every size up to 160 at every pair of alignments, apart and overlapping from below: accepted, and as the model.
void SmallCopies(Function f) {
  Pair p(1 + f);
  for (uint32_t size = 0; size <= 160; ++size) {
    for (uint32_t dest_align = 0; dest_align < 16; ++dest_align) {
      for (uint32_t source_align = 0; source_align < 16; ++source_align) {
        const uint32_t dest = 0x1000 + dest_align, source = 0x3000 + source_align;
        CHECK(Call(p, f, dest, source, size));
        CHECK(p.Same(dest, dest + size));
      }
    }
    // The destination below the source, the two overlapping by every distance: a forward copy, as memmove.
    for (uint32_t distance = 0; distance <= 140; ++distance) {
      const uint32_t dest = 0x8000 + (size & 7), source = dest + distance;
      CHECK(Call(p, f, dest, source, size));
      CHECK(p.Same(dest, source + size));
    }
  }
  CHECK(p.SameEverywhere());
}

// The destination inside the source: declined, nothing written (the stack slot neither).
void DeclinedCopies(Function f) {
  Pair p(11 + f);
  for (uint32_t size = 2; size <= 160; ++size) {
    for (uint32_t distance = 1; distance < size; ++distance) {
      CHECK(!Call(p, f, 0x4000 + distance, 0x4000, size));
    }
  }
  CHECK(p.SameEverywhere());
  // Not plain memory: declined before anything is touched (there is no memory behind these addresses here).
  const Memory nowhere{};
  for (uint32_t address : {0x7F000000u, 0x7FC80000u, 0x7EFFFFF8u, 0xDFFFFFF8u, 0xFFFFFFF8u}) {
    if (f == kMemcpy) {
      CHECK(!xdk::CrtMemcpy(nowhere, address, 0x1000, 64, kStack));
      CHECK(!xdk::CrtMemcpy(nowhere, 0x1000, address, 64, kStack));
    } else {
      CHECK(!xdk::CrtCopyForward(nowhere, address, 0x1000, 64));
      CHECK(!xdk::CrtCopyForward(nowhere, 0x1000, address, 64));
    }
  }
}

void Fills() {
  Pair p(21);
  for (uint32_t size = 0; size <= 300; ++size) {
    for (uint32_t align = 0; align < 16; ++align) {
      const uint32_t dest = 0x2000 + align;
      // Only the low byte of the value counts.
      CHECK(Call(p, kMemset, dest, 0x12345600u | (size * 7 + align), size));
      CHECK(p.Same(dest, dest + size));
    }
  }
  CHECK(p.SameEverywhere());
  const Memory nowhere{};
  for (uint32_t address : {0x7F000000u, 0x7FC80000u, 0x7EFFFFF8u, 0xDFFFFFF8u, 0xFFFFFFF8u}) {
    CHECK(!xdk::CrtMemset(nowhere, address, 0, 64));
  }
}

// Calls at random: sizes mostly small, some up to 64 KB; a call is accepted exactly when the header says so.
void Random(uint32_t seed, uint32_t count) {
  Pair p(seed);
  std::mt19937 rng(seed * 7919u);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t kind = rng() % 8;
    const uint32_t size = kind < 3 ? rng() % 64 : kind < 5 ? rng() % 200 : kind < 7 ? rng() % 2048 : rng() % 65536;
    const uint32_t limit = kStack - 64 - size;
    const uint32_t dest = 64 + rng() % (limit - 64);
    // Half of the copies near their destination, so that overlaps of every kind come up.
    uint32_t source = 64 + rng() % (limit - 64);
    if (rng() & 1) {
      const int32_t delta = int32_t(rng() % 129) - 64;
      const int64_t near = int64_t(dest) + delta;
      if (near >= 64 && near < limit) source = uint32_t(near);
    }
    const Function f = Function(rng() % 3);
    const uint32_t second = f == kMemset ? rng() : source;
    // One call in four with a destination register whose high half is not zero.
    const bool accepted = Call(p, f, dest, second, size, rng() % 4 ? 0 : rng());
    CHECK(accepted == (f == kMemset || xdk::IsPlainForwardCopy(dest, source, size)));
    CHECK(p.Same(dest, dest + size));
    if (i % 4096 == 0) CHECK(p.SameEverywhere());
  }
  CHECK(p.SameEverywhere());
}

// memcpy saves the 64 bits of the destination's register, not the address alone: found in game (Test Drive
// Unlimited calls it with a register whose high half its own arithmetic left set), kept here.
void SavedRegister() {
  Pair p(31);
  CHECK(Call(p, kMemcpy, 0x1000, 0x2000, 13, 0xDEADBEEFu));
  const uint8_t slot[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0x10, 0};
  CHECK(std::memcmp(&p.native[kStack - 8], slot, 8) == 0);
  CHECK(std::memcmp(&p.native[0x1000], &p.native[0x2000], 13) == 0);
  CHECK(p.SameEverywhere());
  // The address is the register's low half whatever the high half holds: still declined where it should be.
  CHECK(!Call(p, kMemcpy, 0x4004, 0x4000, 64, 1));
  CHECK(p.SameEverywhere());
}

// The physical view (guest addresses from 0xE0000000) at its host offset; the rest of memory at none.
void PhysicalView() {
  constexpr uint32_t kOffset = 0x1000, kBytes = 0x8000;
  std::vector<uint8_t> view(kBytes + kOffset), low(kBytes);
  for (uint32_t i = 0; i < kBytes; ++i) low[i] = uint8_t(i * 13 + 1);
  // One base cannot serve both buffers: each memory maps the one its test touches.
  Memory physical;
  physical.base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(view.data()) - 0xE0000000u);
  physical.physical_offset = kOffset;
  CHECK(physical.Host(0xE0000010u) == view.data() + kOffset + 0x10);
  CHECK(xdk::CrtMemset(physical, 0xE0000100u, 0x5A, 300));
  bool filled = true;
  for (uint32_t i = 0; i < 300; ++i) filled = filled && view[kOffset + 0x100 + i] == 0x5A;
  CHECK(filled && view[kOffset + 0x100 - 1] == 0 && view[kOffset + 0x100 + 300] == 0);
  CHECK(xdk::CrtCopyForward(physical, 0xE0001000u, 0xE0000100u, 300));
  CHECK(std::memcmp(&view[kOffset + 0x1000], &view[kOffset + 0x100], 300) == 0);
  Memory plain;
  plain.base = low.data();
  plain.physical_offset = kOffset;
  CHECK(plain.Host(0x10) == low.data() + 0x10);
  CHECK(xdk::CrtMemcpy(plain, 0x2000, 0x100, 500, 0x7000));
  CHECK(std::memcmp(&low[0x2000], &low[0x100], 500) == 0);
  const uint8_t slot[8] = {0, 0, 0, 0, 0, 0, 0x20, 0};
  CHECK(std::memcmp(&low[0x7000 - 8], slot, 8) == 0);
}

}  // namespace

int main() {
  Predicates();
  for (Function f : {kMemcpy, kCopyForward}) {
    SmallCopies(f);
    DeclinedCopies(f);
  }
  Fills();
  SavedRegister();
  for (uint32_t seed = 1; seed <= 4; ++seed) Random(seed, 100000);
  PhysicalView();
  std::printf("xdk_crt_tests: %llu checks, %d failing\n", static_cast<unsigned long long>(g_checks), g_failures);
  return g_failures ? 1 : 0;
}
