#pragma once
// Open-addressing hash map from uint64 keys to uint32 values.
//
// Built for one job: "order reference number -> slot in the order pool".
// std::unordered_map allocates a node per entry and chases a pointer on every
// lookup (a likely cache miss). Here keys and values sit side by side in one
// flat array, so a lookup is usually a single cache line.
//
// Design choices worth knowing for interviews:
//  * Linear probing: on collision, try the next slot. The CPU prefetcher loves
//    this; it beats fancier schemes at load factors <= ~0.7.
//  * Power-of-two capacity: index = hash & (cap - 1) instead of a slow modulo.
//  * Fibonacci hashing: multiply by 2^64/phi. Order refs are nearly sequential,
//    and the multiply spreads neighbours across the table.
//  * Backward-shift deletion: instead of leaving "tombstones" that slow down
//    later lookups, we shift following entries back to close the gap.
//  * Reserve up front: a rehash copies the whole table, which would be a
//    multi-millisecond latency spike in the middle of the trading day.

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace hft {

class FlatHashMap {
public:
    static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};  // reserved key
    static constexpr std::uint32_t kNotFound = ~std::uint32_t{0};

    explicit FlatHashMap(std::size_t expected = 1024) { reserve(expected); }

    // Make room for `n` entries without rehashing (max load factor 0.5).
    void reserve(std::size_t n) {
        std::size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        if (cap > slots_.size()) rehash(cap);
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

    // Insert if absent. Returns false, leaving the map unchanged, if the key
    // already exists (same contract as std::map::insert).
    bool insert(std::uint64_t key, std::uint32_t value) {
        if (key == kEmpty) throw std::invalid_argument("reserved key");
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        std::size_t i = home(key);
        while (true) {
            Slot& s = slots_[i];
            if (s.key == kEmpty) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
            if (s.key == key) return false;
            i = (i + 1) & mask_;
        }
    }

    std::uint32_t find(std::uint64_t key) const noexcept {
        std::size_t i = home(key);
        while (true) {
            const Slot& s = slots_[i];
            if (s.key == key) return s.value;
            if (s.key == kEmpty) return kNotFound;
            i = (i + 1) & mask_;
        }
    }

    // Remove `key`; returns its value, or kNotFound.
    std::uint32_t erase(std::uint64_t key) noexcept {
        std::size_t i = home(key);
        while (true) {
            Slot& s = slots_[i];
            if (s.key == kEmpty) return kNotFound;
            if (s.key == key) break;
            i = (i + 1) & mask_;
        }
        const std::uint32_t value = slots_[i].value;

        // Backward-shift: walk forward and pull back any entry whose home slot
        // is at or before the hole, so every remaining key stays reachable.
        std::size_t hole = i;
        std::size_t j = (i + 1) & mask_;
        while (slots_[j].key != kEmpty) {
            const std::size_t h = home(slots_[j].key);
            // Is `h` cyclically outside (hole, j]? Then the entry may move to the hole.
            const bool movable = (hole <= j) ? (h <= hole || h > j) : (h <= hole && h > j);
            if (movable) {
                slots_[hole] = slots_[j];
                hole = j;
            }
            j = (j + 1) & mask_;
        }
        slots_[hole].key = kEmpty;
        --size_;
        return value;
    }

    void clear() noexcept {
        for (auto& s : slots_) s.key = kEmpty;
        size_ = 0;
    }

private:
    struct Slot {
        std::uint64_t key = kEmpty;
        std::uint32_t value = 0;
    };

    std::size_t home(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        shift_ = 64 - __builtin_ctzll(new_cap);
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != kEmpty) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace hft
