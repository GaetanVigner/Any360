// FlatKeyMap (any360/gfx/flat_key_map.h), the renderer's descriptor-set caches: every key found with its value and
// every absent key not found, against std::map, over keys of 1 to 33 words (a texture set's layout, 16 views, 16
// samplers) with shared prefixes, through growth and clears; refilled after a clear, it allocates nothing. Then the
// cost of a batch's lookups and inserts against the std::unordered_map of std::vector keys it replaced (time and
// allocations a batch).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "any360/gfx/flat_key_map.h"
#include "any360/sys/alloc_meter.h"

namespace {

int g_failures = 0;
#define CHECK(condition)                                          \
  do {                                                            \
    if (!(condition)) {                                           \
      std::printf("%s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using Key = std::vector<uint64_t>;

// `count` distinct keys: a layout word from 4, then views and samplers from small pools (sets share most handles).
std::vector<Key> MakeKeys(std::mt19937_64& rng, size_t count) {
  std::map<Key, int> seen;
  std::vector<Key> keys;
  while (keys.size() < count) {
    Key k;
    k.push_back(0x1000 + rng() % 4);
    const uint32_t extra = uint32_t(rng() % 33);
    for (uint32_t i = 0; i < extra; ++i) k.push_back(0x20000 + rng() % 24);
    if (seen.emplace(k, 0).second) keys.push_back(std::move(k));
  }
  return keys;
}

struct OldHash {
  size_t operator()(const Key& key) const {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t v : key) h = (h ^ v) * 0x100000001B3ull;
    return size_t(h ^ (h >> 29));
  }
};

}  // namespace

int main(int argc, char** argv) {
  const bool bench = argc > 1 && !std::strcmp(argv[1], "--bench");
  std::mt19937_64 rng(42);
  any360::gfx::FlatKeyMap map;
  CHECK(map.Find(nullptr, 1) == nullptr && map.size() == 0);
  for (int round = 0; round < 6; ++round) {
    const std::vector<Key> keys = MakeKeys(rng, size_t(50) << round % 4);
    const std::vector<Key> absent = MakeKeys(rng, 200);
    std::map<Key, uint64_t> reference;
    for (size_t i = 0; i < keys.size(); i += 2) {
      map.Insert(keys[i].data(), uint32_t(keys[i].size()), 7 * i + 1);
      reference[keys[i]] = 7 * i + 1;
    }
    CHECK(map.size() == reference.size());
    uint32_t wrong = 0;
    for (const Key& k : keys) {
      const uint64_t* found = map.Find(k.data(), uint32_t(k.size()));
      const auto it = reference.find(k);
      wrong += it == reference.end() ? found != nullptr : !found || *found != it->second;
    }
    for (const Key& k : absent) wrong += !reference.count(k) && map.Find(k.data(), uint32_t(k.size())) != nullptr;
    CHECK(wrong == 0);
    // A prefix of a stored key is another key.
    const Key& longest = *std::max_element(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.size() < b.size(); });
    if (longest.size() > 1 && !reference.count(Key(longest.begin(), longest.end() - 1))) {
      CHECK(map.Find(longest.data(), uint32_t(longest.size() - 1)) == nullptr);
    }
    map.Clear();
    CHECK(map.size() == 0);
    for (const Key& k : keys) CHECK(map.Find(k.data(), uint32_t(k.size())) == nullptr);
  }
  // Refilled with as many keys as it held before: no allocation.
  {
    const std::vector<Key> keys = MakeKeys(rng, 400);
    any360::gfx::FlatKeyMap warm;
    for (size_t i = 0; i < keys.size(); ++i) warm.Insert(keys[i].data(), uint32_t(keys[i].size()), i);
    warm.Clear();
    const auto before = any360::sys::ThreadAllocations();
    for (size_t i = 0; i < keys.size(); ++i) warm.Insert(keys[i].data(), uint32_t(keys[i].size()), i);
    uint64_t sum = 0;
    for (const Key& k : keys) sum += *warm.Find(k.data(), uint32_t(k.size()));
    const auto spent = any360::sys::ThreadAllocations() - before;
    std::printf("gfx_flat_key_map_tests: 400 keys refilled after a clear: %llu allocations, %llu bytes\n",
                (unsigned long long)spent.count, (unsigned long long)spent.bytes);
    CHECK(spent.count == 0 && sum == keys.size() * (keys.size() - 1) / 2);
  }
  // A batch as the renderer makes it: 2,500 draws' lookups over 80 distinct sets (inserted when missing), then the
  // batch's clear; the batch after reuses the map.
  {
    const std::vector<Key> sets = MakeKeys(rng, 80);
    std::vector<uint32_t> draws(2500);
    for (uint32_t& d : draws) d = uint32_t(rng() % sets.size());
    const int batches = bench ? 2000 : 50;
    auto run_flat = [&](double& ns, double& allocs) {
      any360::gfx::FlatKeyMap m;
      const auto a0 = any360::sys::ThreadAllocations();
      const auto t0 = std::chrono::steady_clock::now();
      uint64_t made = 0;
      for (int b = 0; b < batches; ++b) {
        for (uint32_t d : draws) {
          const Key& k = sets[d];
          if (!m.Find(k.data(), uint32_t(k.size()))) m.Insert(k.data(), uint32_t(k.size()), ++made);
        }
        m.Clear();
      }
      ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / batches;
      allocs = double((any360::sys::ThreadAllocations() - a0).count) / batches;
      return made;
    };
    auto run_old = [&](double& ns, double& allocs) {
      std::unordered_map<Key, uint64_t, OldHash> m;
      Key key;
      const auto a0 = any360::sys::ThreadAllocations();
      const auto t0 = std::chrono::steady_clock::now();
      uint64_t made = 0;
      for (int b = 0; b < batches; ++b) {
        for (uint32_t d : draws) {
          key.assign(sets[d].begin(), sets[d].end());
          if (m.find(key) == m.end()) m.emplace(key, ++made);
        }
        m.clear();
      }
      ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / batches;
      allocs = double((any360::sys::ThreadAllocations() - a0).count) / batches;
      return made;
    };
    double flat_ns = 0, flat_allocs = 0, old_ns = 0, old_allocs = 0;
    const uint64_t made_flat = run_flat(flat_ns, flat_allocs), made_old = run_old(old_ns, old_allocs);
    CHECK(made_flat == made_old && made_flat == uint64_t(batches) * sets.size());
    CHECK(flat_allocs < 1.0);
    std::printf("gfx_flat_key_map_tests: a batch of 2500 lookups over 80 sets: FlatKeyMap %.1f us and %.2f allocations, "
                "unordered_map of vectors %.1f us and %.1f allocations (%.2fx)\n",
                flat_ns / 1e3, flat_allocs, old_ns / 1e3, old_allocs, old_ns / flat_ns);
  }
  std::printf("gfx_flat_key_map_tests: %d failing checks\n", g_failures);
  return g_failures ? 1 : 0;
}
