// A map from a short sequence of 64-bit words to a 64-bit value, for caches emptied and refilled every frame or batch
// (descriptor sets by their contents): open addressing over one array of slots, the keys' words in one array beside
// it. Clear keeps both arrays' capacity, so a cache refilled with as many keys as before allocates nothing.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace any360::gfx {

class FlatKeyMap {
 public:
  // The value stored for `key` (`words` words), or nullptr.
  const uint64_t* Find(const uint64_t* key, uint32_t words) const {
    if (slots_.empty()) return nullptr;
    const uint64_t hash = Hash(key, words);
    for (size_t i = size_t(hash) & (slots_.size() - 1);; i = (i + 1) & (slots_.size() - 1)) {
      const Slot& s = slots_[i];
      if (!s.words) return nullptr;
      if (s.hash == hash && s.words == words && std::memcmp(&words_[s.offset], key, 8 * size_t(words)) == 0) return &s.value;
    }
  }
  // Stores `value` for `key`, which must not be in the map yet.
  void Insert(const uint64_t* key, uint32_t words, uint64_t value) {
    if (2 * (count_ + 1) > slots_.size()) Grow();
    const uint64_t hash = Hash(key, words);
    Place({hash, uint32_t(words_.size()), words, value});
    words_.insert(words_.end(), key, key + words);
    ++count_;
  }
  void Clear() {
    if (!count_) return;
    for (Slot& s : slots_) s.words = 0;
    words_.clear();
    count_ = 0;
  }
  size_t size() const { return count_; }

 private:
  struct Slot {
    uint64_t hash = 0;
    uint32_t offset = 0, words = 0;  // words 0: empty (a key has at least one word)
    uint64_t value = 0;
  };
  static uint64_t Hash(const uint64_t* key, uint32_t words) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < words; ++i) h = (h ^ key[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
  }
  void Place(const Slot& slot) {
    size_t i = size_t(slot.hash) & (slots_.size() - 1);
    while (slots_[i].words) i = (i + 1) & (slots_.size() - 1);
    slots_[i] = slot;
  }
  void Grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.empty() ? 64 : 2 * old.size(), Slot{});
    for (const Slot& s : old) {
      if (s.words) Place(s);
    }
  }
  std::vector<Slot> slots_;
  std::vector<uint64_t> words_;
  size_t count_ = 0;
};

}  // namespace any360::gfx
