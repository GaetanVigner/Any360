// U32Map: a map from 32-bit keys (guest addresses, handles) to 32-bit values in one flat array, open addressing with
// linear probing and backward-shift removal (no tombstones). It allocates only when it grows past half full, so a
// table that has reached its working size inserts and erases without touching the host heap (std::map and
// std::unordered_map allocate a node an insertion: the object manager paid one per object freed on close).
// Not thread-safe; the owner locks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace any360::sys {

class U32Map {
 public:
  explicit U32Map(size_t capacity = 64) { Rehash(RoundUp(capacity * 2)); }

  // The value of `key`, or null.
  uint32_t* Find(uint32_t key) {
    if (key == kEmpty) return has_empty_key_ ? &empty_key_value_ : nullptr;
    for (size_t i = Slot(key);; i = (i + 1) & mask_) {
      if (slots_[i].key == key) return &slots_[i].value;
      if (slots_[i].key == kEmpty) return nullptr;
    }
  }
  const uint32_t* Find(uint32_t key) const { return const_cast<U32Map*>(this)->Find(key); }
  bool Contains(uint32_t key) const { return Find(key) != nullptr; }

  // The value of `key`, inserted as 0 when absent.
  uint32_t& operator[](uint32_t key) {
    if (key == kEmpty) {
      if (!has_empty_key_) has_empty_key_ = true, empty_key_value_ = 0, ++size_;
      return empty_key_value_;
    }
    if ((size_ + 1) * 2 > slots_.size()) Rehash(slots_.size() * 2);
    size_t i = Slot(key);
    for (; slots_[i].key != kEmpty; i = (i + 1) & mask_) {
      if (slots_[i].key == key) return slots_[i].value;
    }
    slots_[i] = {key, 0};
    ++size_;
    return slots_[i].value;
  }

  // Whether `key` was there.
  bool Erase(uint32_t key) {
    if (key == kEmpty) {
      if (!has_empty_key_) return false;
      has_empty_key_ = false, --size_;
      return true;
    }
    size_t i = Slot(key);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == key) break;
      if (slots_[i].key == kEmpty) return false;
    }
    // Backward shift: each later entry of the run moves into the hole when its home slot does not lie between the
    // hole and itself (cyclically), so every lookup still finds its key before an empty slot.
    for (size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
      if (slots_[j].key == kEmpty) break;
      const size_t home = Slot(slots_[j].key);
      if (((j - home) & mask_) >= ((j - i) & mask_)) {
        slots_[i] = slots_[j];
        i = j;
      }
    }
    slots_[i].key = kEmpty;
    --size_;
    return true;
  }

  // Empties the map and keeps its storage.
  void Clear() {
    for (Entry& e : slots_) e.key = kEmpty;
    has_empty_key_ = false;
    size_ = 0;
  }
  size_t size() const { return size_; }
  size_t capacity() const { return slots_.size() / 2; }  // entries it holds before it grows

 private:
  static constexpr uint32_t kEmpty = 0;  // key 0 is kept apart (has_empty_key_)
  struct Entry {
    uint32_t key = kEmpty, value = 0;
  };
  static size_t RoundUp(size_t n) {
    size_t p = 16;
    while (p < n) p *= 2;
    return p;
  }
  // Fibonacci hashing: the product's top bits, so aligned guest addresses spread over the whole table.
  size_t Slot(uint32_t key) const { return uint32_t(key * 0x9E3779B1u) >> shift_; }
  void Rehash(size_t slots) {
    std::vector<Entry> old;
    old.swap(slots_);
    slots_.assign(slots, Entry{});
    mask_ = slots - 1;
    shift_ = 32;
    for (size_t n = slots; n > 1; n >>= 1) --shift_;
    for (const Entry& e : old) {
      if (e.key == kEmpty) continue;
      size_t i = Slot(e.key);
      while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
      slots_[i] = e;
    }
  }

  std::vector<Entry> slots_;
  size_t mask_ = 0, size_ = 0;
  uint32_t shift_ = 32;
  bool has_empty_key_ = false;
  uint32_t empty_key_value_ = 0;
};

}  // namespace any360::sys
