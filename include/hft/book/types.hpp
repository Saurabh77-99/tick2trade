#pragma once
// Core order book types.
//
// We store every resting order (an "L3" or "market-by-order" book), not just
// the total per price ("L2" / "market-by-price"). ITCH messages refer to
// orders by reference number, so we need per-order state to apply them. It
// also gives us each order's place in its price queue, which Phase 5 needs
// to simulate when *our* orders would get filled.
//
// Orders live in a pool (one contiguous vector) and are linked into a FIFO
// per price level by 32-bit indices instead of pointers:
//   * indices stay valid when the vector grows; pointers would dangle
//   * 4 bytes instead of 8 keeps Order at exactly 32 bytes = 2 per cache line

#include <cstdint>
#include <vector>

#include "hft/itch/messages.hpp"

namespace hft::book {

using itch::Price;
using itch::Side;

inline constexpr std::uint32_t kNil = ~std::uint32_t{0};

struct Order {
    std::uint64_t ref;      // exchange order reference number
    Price price;
    std::uint32_t shares;   // remaining (open) shares
    std::uint32_t prev;     // previous order in the price level's FIFO
    std::uint32_t next;     // next order in the FIFO
    std::uint16_t locate;   // which symbol
    Side side;
};
static_assert(sizeof(Order) == 32, "Order should pack into half a cache line");

struct Level {
    Price price;
    std::uint32_t count;    // number of orders resting here
    std::uint64_t qty;      // total shares
    std::uint32_t head;     // oldest order (first to fill)
    std::uint32_t tail;     // newest order
};

// Free-list object pool. alloc()/release() are O(1) and never touch the heap
// once the vector has grown to the day's peak number of live orders.
class OrderPool {
public:
    void reserve(std::size_t n) {
        slots_.reserve(n);
        free_.reserve(n);
    }

    std::uint32_t alloc() {
        if (!free_.empty()) {
            const std::uint32_t idx = free_.back();
            free_.pop_back();
            return idx;
        }
        slots_.emplace_back();
        return static_cast<std::uint32_t>(slots_.size() - 1);
    }

    void release(std::uint32_t idx) { free_.push_back(idx); }

    Order& operator[](std::uint32_t idx) noexcept { return slots_[idx]; }
    const Order& operator[](std::uint32_t idx) const noexcept { return slots_[idx]; }

    std::size_t live() const noexcept { return slots_.size() - free_.size(); }
    std::size_t high_water() const noexcept { return slots_.size(); }

private:
    std::vector<Order> slots_;
    std::vector<std::uint32_t> free_;
};

// True if price `a` is more aggressive than `b` for side S
// (higher for bids, lower for asks).
template <Side S>
constexpr bool better(Price a, Price b) noexcept {
    if constexpr (S == Side::Buy) return a > b;
    else return a < b;
}

}  // namespace hft::book
