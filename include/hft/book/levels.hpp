#pragma once
// Two interchangeable containers for one side of the book's price levels.
//
// Both expose the same interface, so OrderBook can be built with either and
// the benchmark can compare them on identical data:
//
//   Level*       find(Price)            nullptr if absent
//   Level&       find_or_insert(Price)  new levels start empty
//   void         erase(Price)
//   const Level* best() const           nullptr if the side is empty
//   for_each(n, fn)                     best level first, at most n levels
//   size()
//
// 1) MapLevels: std::map (a red-black tree). O(log n) everything, but every
//    level is a separate heap node, so walking or searching jumps around
//    memory. It's the textbook answer.
//
// 2) VectorLevels: a sorted std::vector with the BEST price at the BACK.
//    Searches scan linearly from the back. That sounds worse, but real order
//    flow is concentrated within a few ticks of the top of book, so the scan
//    usually stops after 1-3 contiguous, prefetched elements. Inserting or
//    erasing near the top shifts only the few elements behind it. This is
//    the layout many production books use. Run the benchmark to check it.

#include <cstddef>
#include <functional>
#include <map>
#include <type_traits>
#include <vector>

#include "hft/book/types.hpp"

namespace hft::book {

template <Side S>
class VectorLevels {
public:
    Level* find(Price p) noexcept {
        for (std::size_t i = levels_.size(); i-- > 0;) {
            Level& lv = levels_[i];
            if (lv.price == p) return &lv;
            if (better<S>(p, lv.price)) return nullptr;  // passed where it would be
        }
        return nullptr;
    }

    Level& find_or_insert(Price p) {
        std::size_t i = levels_.size();
        while (i > 0) {
            Level& lv = levels_[i - 1];
            if (lv.price == p) return lv;
            if (better<S>(p, lv.price)) break;  // insert just above this level
            --i;
        }
        return *levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i),
                               Level{p, 0, 0, kNil, kNil});
    }

    void erase(Price p) noexcept {
        for (std::size_t i = levels_.size(); i-- > 0;) {
            if (levels_[i].price == p) {
                levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    const Level* best() const noexcept { return levels_.empty() ? nullptr : &levels_.back(); }

    template <typename F>
    void for_each(std::size_t n, F&& fn) const {
        for (std::size_t i = levels_.size(); i-- > 0 && n-- > 0;) fn(levels_[i]);
    }

    std::size_t size() const noexcept { return levels_.size(); }

private:
    std::vector<Level> levels_;  // worst ... best
};

template <Side S>
class MapLevels {
    using Cmp = std::conditional_t<S == Side::Buy, std::greater<Price>, std::less<Price>>;

public:
    Level* find(Price p) noexcept {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }

    Level& find_or_insert(Price p) {
        return levels_.try_emplace(p, Level{p, 0, 0, kNil, kNil}).first->second;
    }

    void erase(Price p) noexcept { levels_.erase(p); }

    const Level* best() const noexcept {
        return levels_.empty() ? nullptr : &levels_.begin()->second;
    }

    template <typename F>
    void for_each(std::size_t n, F&& fn) const {
        for (auto it = levels_.begin(); it != levels_.end() && n-- > 0; ++it) fn(it->second);
    }

    std::size_t size() const noexcept { return levels_.size(); }

private:
    std::map<Price, Level, Cmp> levels_;  // best first
};

// Policies select a container for both sides at once: OrderBook<VectorPolicy>.
struct VectorPolicy {
    template <Side S>
    using Levels = VectorLevels<S>;
    static constexpr const char* name = "vector";
};

struct MapPolicy {
    template <Side S>
    using Levels = MapLevels<S>;
    static constexpr const char* name = "map";
};

}  // namespace hft::book
