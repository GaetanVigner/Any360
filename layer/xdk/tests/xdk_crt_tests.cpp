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
#include "xdk_test.h"

namespace {

using any360::xdk::Memory;
using xdk_test::kStack;
using xdk_test::Pair;
namespace xdk = any360::xdk;

enum Function { kMemcpy, kCopyForward, kMemset, kMemmove };

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
    case kMemmove:
      accepted = xdk::CrtMemmove(p.a, uint64_t(upper) << 32 | dest, source_or_value, size, kStack);
      if (accepted) xdk::reference::Memmove(p.b, uint64_t(upper) << 32 | dest, source_or_value, size, kStack);
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
  // The functions' own tests are those. memmove compares its two addresses as signed: a destination from
  // 0x80000000 is "below" a source under it, and takes memcpy's side.
  CHECK(xdk::CrtMemcpyAccepts(100, 200, 50) && !xdk::CrtMemcpyAccepts(120, 100, 50) && !xdk::CrtMemcpyAccepts(0x7F000000, 100, 4));
  CHECK(xdk::CrtMemsetAccepts(100, 50) && !xdk::CrtMemsetAccepts(0x7FC80000, 4));
  CHECK(xdk::CrtMemmoveAccepts(100, 100, 50) && xdk::CrtMemmoveAccepts(0x7F000000, 0x7F000000, 4));
  CHECK(xdk::CrtMemmoveAccepts(120, 100, 50) && xdk::CrtMemmoveAccepts(100, 120, 50));
  CHECK(xdk::CrtMemmoveAccepts(0x80001000, 0x1000, 64) && xdk::CrtMemmoveAccepts(0x1000, 0x80001000, 64));
  CHECK(!xdk::CrtMemmoveAccepts(0x7F000000, 100, 4) && !xdk::CrtMemmoveAccepts(100, 0x7F000000, 4));
  CHECK(!xdk::CrtMemmoveAccepts(0x7EFFFFF0, 0x80000000, 0x20) && !xdk::CrtMemmoveAccepts(0x80000000, 0x7EFFFFF0, 0x20));
}

// memmove: every size up to 160 with the source every distance below and above the destination. All accepted; the
// stack slot is written exactly when the destination is below the source (memcpy's side), as the model does.
void Moves() {
  Pair p(41);
  for (uint32_t size = 0; size <= 160; ++size) {
    for (int32_t distance = -170; distance <= 170; ++distance) {
      const uint32_t dest = 0x8000 + (size & 15), source = uint32_t(int32_t(dest) + distance);
      CHECK(Call(p, kMemmove, dest, source, size, size & 1 ? 0x12345678u : 0));
      CHECK(p.Same(dest < source ? dest : source, (dest < source ? source : dest) + size));
    }
    CHECK(p.SameEverywhere());
  }
  // Long ones, the two ranges overlapping by a little and by most of their length.
  for (uint32_t size : {1000u, 4096u, 65537u}) {
    for (int32_t distance : {-1, 1, -7, 7, -129, 129, -int32_t(size) + 1, int32_t(size) - 1, -int32_t(size), int32_t(size)}) {
      const uint32_t dest = 0x40000, source = uint32_t(int32_t(dest) + distance);
      CHECK(Call(p, kMemmove, dest, source, size));
    }
    CHECK(p.SameEverywhere());
  }
  // Not plain memory: declined on both sides, nothing touched.
  const Memory nowhere{};
  for (uint32_t address : {0x7F000000u, 0x7FC80000u, 0x7EFFFFF8u}) {
    CHECK(!xdk::CrtMemmove(nowhere, address, 0x1000, 64, kStack));
    CHECK(!xdk::CrtMemmove(nowhere, 0x1000, address, 64, kStack));
  }
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
    const Function f = Function(rng() % 4);
    const uint32_t second = f == kMemset ? rng() : source;
    // One call in four with a destination register whose high half is not zero.
    const bool accepted = Call(p, f, dest, second, size, rng() % 4 ? 0 : rng());
    CHECK(accepted == (f == kMemset || f == kMemmove || xdk::IsPlainForwardCopy(dest, source, size)));
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

// A string of `length` bytes that are never zero (nor `avoid`), then a terminator, at `address` of both memories.
void PutString(Pair& p, uint32_t address, uint32_t length, std::mt19937& rng, uint8_t avoid = 0) {
  for (uint32_t i = 0; i <= length; ++i) {
    uint8_t byte = i == length ? 0 : uint8_t(1 + rng() % 255);
    if (i != length && byte == avoid) byte = uint8_t(avoid == 1 ? 2 : 1);
    p.native[address + i] = p.model[address + i] = byte;
  }
}

// wcslen, strchr and strncmp only read: the value each leaves in r3 against the model's, nothing written.
void StringReads() {
  Pair p(51);
  std::mt19937 rng(52);
  uint64_t r3 = 0;
  // wcslen: every length up to 300 at even and odd addresses.
  for (uint32_t length = 0; length <= 300; ++length) {
    for (uint32_t offset = 0; offset < 4; ++offset) {
      const uint32_t at = 0x1000 + offset;
      for (uint32_t i = 0; i < length; ++i) {
        const uint16_t unit = uint16_t(1 + rng() % 0xFFFF);
        p.native[at + 2 * i] = p.model[at + 2 * i] = uint8_t(unit >> 8);
        p.native[at + 2 * i + 1] = p.model[at + 2 * i + 1] = uint8_t(unit);
      }
      p.native[at + 2 * length] = p.model[at + 2 * length] = 0;
      p.native[at + 2 * length + 1] = p.model[at + 2 * length + 1] = 0;
      CHECK(xdk::CrtWcslen(p.a, at, r3) && r3 == length && r3 == xdk::reference::Wcslen(p.b, at));
    }
  }
  // strchr: a byte that is there at every position, one that is not, the terminator, values no byte equals; the
  // register's high half kept in the address found.
  for (uint32_t length = 0; length <= 200; ++length) {
    const uint32_t at = 0x4000 + (length & 7);
    PutString(p, at, length, rng, 0x41);
    const uint64_t reg = 0xABCD000000000000ull | at;
    CHECK(xdk::CrtStrchr(p.a, reg, 0x41, r3) && r3 == 0 && xdk::reference::Strchr(p.b, reg, 0x41) == 0);
    CHECK(xdk::CrtStrchr(p.a, reg, 0, r3) && r3 == reg + length && r3 == xdk::reference::Strchr(p.b, reg, 0));
    for (uint32_t character : {0x141u, 0xFFFFFF41u, 0x80000000u}) {
      CHECK(xdk::CrtStrchr(p.a, reg, character, r3) && r3 == 0 && xdk::reference::Strchr(p.b, reg, character) == 0);
    }
    for (uint32_t where = 0; where < length; where += 1 + length / 16) {
      p.native[at + where] = p.model[at + where] = 0x41;
      CHECK(xdk::CrtStrchr(p.a, reg, 0x41, r3) && r3 == reg + where && r3 == xdk::reference::Strchr(p.b, reg, 0x41));
      p.native[at + where] = p.model[at + where] = 0x42;
    }
  }
  // strncmp: equal strings, a difference at every position (both signs), every count around it, counts that are
  // not positive, strings that end before the count.
  for (uint32_t length = 0; length <= 120; ++length) {
    const uint32_t left = 0x8000 + (length & 3), right = 0xA000 + (length & 5);
    PutString(p, left, length, rng);
    std::memcpy(&p.native[right], &p.native[left], length + 1);
    std::memcpy(&p.model[right], &p.model[left], length + 1);
    for (uint32_t count : {0u, 1u, length, length + 1, length + 50, 0x80000000u, 0xFFFFFFFFu}) {
      CHECK(xdk::CrtStrncmp(p.a, left, right, count, r3) && r3 == 0 && xdk::reference::Strncmp(p.b, left, right, count) == 0);
    }
    for (uint32_t where = 0; where < length; where += 1 + length / 12) {
      const uint8_t was = p.native[right + where];
      for (uint8_t other : {uint8_t(was == 255 ? 254 : was + 1), uint8_t(was == 1 ? 2 : was - 1), uint8_t(0)}) {
        p.native[right + where] = p.model[right + where] = other;
        for (uint32_t count : {where, where + 1, length + 5}) {
          CHECK(xdk::CrtStrncmp(p.a, left, right, count, r3) && r3 == xdk::reference::Strncmp(p.b, left, right, count));
          CHECK(count <= where ? r3 == 0 : r3 == uint64_t(int64_t(was) - int64_t(other)));
        }
      }
      p.native[right + where] = p.model[right + where] = was;
    }
  }
  CHECK(p.SameEverywhere());
  // A string in the GPU's register window: declined.
  const Memory nowhere{};
  CHECK(!xdk::CrtWcslen(nowhere, 0x7FC80000, r3) && !xdk::CrtStrchr(nowhere, 0x7F000000, 0x41, r3));
  CHECK(!xdk::CrtStrncmp(nowhere, 0x7FC80000, 0x1000, 4, r3) && xdk::CrtStrncmp(nowhere, 0x7FC80000, 0x1000, 0, r3));
}

// strncpy: every source length against every count around it, apart and with the destination inside the source
// (a byte at a time from the first, as its code: the bytes it has just written are the ones it reads next).
void StringCopies() {
  Pair p(61);
  std::mt19937 rng(62);
  for (uint32_t length = 0; length <= 80; ++length) {
    for (uint32_t count : {0u, 1u, length / 2, length, length + 1, length + 2, length + 40}) {
      const uint32_t source = 0x2000 + (length & 7), dest = 0x6000 + (count & 3);
      PutString(p, source, length, rng);
      CHECK(xdk::CrtStrncpy(p.a, dest, source, count));
      xdk::reference::Strncpy(p.b, dest, source, count);
      CHECK(p.Same(dest, dest + count));
      for (int32_t distance : {-9, -1, 1, 2, 7, 33}) {
        const uint32_t inside = uint32_t(int32_t(source) + distance);
        PutString(p, source, length, rng);
        CHECK(xdk::CrtStrncpy(p.a, inside, source, count));
        xdk::reference::Strncpy(p.b, inside, source, count);
        CHECK(p.Same(source < inside ? source : inside, (source < inside ? inside : source) + count + length + 1));
      }
    }
    CHECK(p.SameEverywhere());
  }
  const Memory nowhere{};
  CHECK(!xdk::CrtStrncpy(nowhere, 0x7F000000, 0x1000, 8) && !xdk::CrtStrncpy(nowhere, 0x1000, 0x7EFFFFFC, 8));
  CHECK(xdk::CrtStrncpyAccepts(0x1000, 0x2000, 64) && !xdk::CrtStrncpyAccepts(0xDFFFFFF0, 0x2000, 64));
}

}  // namespace

int main() {
  Predicates();
  for (Function f : {kMemcpy, kCopyForward}) {
    SmallCopies(f);
    DeclinedCopies(f);
  }
  Fills();
  Moves();
  StringReads();
  StringCopies();
  SavedRegister();
  for (uint32_t seed = 1; seed <= 4; ++seed) Random(seed, 100000);
  PhysicalView();
  return xdk_test::Finish("xdk_crt_tests");
}
