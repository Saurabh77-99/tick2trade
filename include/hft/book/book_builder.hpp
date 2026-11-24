#pragma once
// BookBuilder: an ITCH handler that maintains an order book for every symbol.
//
// Feed it to hft::itch::parse() and it applies each message:
//
//   A/F add       -> new order at the tail of its price level's FIFO
//   E   executed  -> reduce the order (trade at the order's resting price)
//   C   exec w/ px-> reduce the order (trade at the message's price)
//   X   cancel    -> reduce the order (partial cancel)
//   D   delete    -> remove the order
//   U   replace   -> remove the old order, add the new one at the back of the
//                    queue (a replace loses time priority, as on the exchange)
//   P/Q trades    -> no book change (hidden orders / auctions); volume only
//
// Errors in the input never crash the builder. They are counted in
// BuilderStats, so replaying a real day doubles as a correctness check:
// a correct builder on a complete file sees zero unknown refs.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hft/book/levels.hpp"
#include "hft/book/types.hpp"
#include "hft/common/flat_hash_map.hpp"
#include "hft/itch/parser.hpp"

namespace hft::book {

template <typename Policy>
struct OrderBook {
    typename Policy::template Levels<Side::Buy> bids;
    typename Policy::template Levels<Side::Sell> asks;

    // Per-symbol trade statistics.
    std::uint64_t volume = 0;        // printable shares traded
    std::uint64_t trades = 0;
    Price last_price = 0;
    std::uint32_t last_shares = 0;

    Price best_bid() const noexcept { return bids.best() ? bids.best()->price : 0; }
    Price best_ask() const noexcept { return asks.best() ? asks.best()->price : 0; }
    bool crossed() const noexcept {
        return bids.best() && asks.best() && bids.best()->price >= asks.best()->price;
    }
};

struct BuilderStats {
    std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
    std::uint64_t unknown_ref = 0;      // exec/cancel/delete for an order we don't have
    std::uint64_t duplicate_ref = 0;    // add with a ref that's already live
    std::uint64_t overfill = 0;         // reduce by more shares than remain
    std::uint64_t crossed_after_add = 0;  // bid >= ask right after an add/replace
};

template <typename Policy = VectorPolicy>
class BookBuilder : public itch::NullHandler {
public:
    using Book = OrderBook<Policy>;

    explicit BookBuilder(std::size_t expected_live_orders = 1 << 20) {
        pool_.reserve(expected_live_orders);
        refs_.reserve(expected_live_orders);
    }

    // ---- ITCH handler callbacks -------------------------------------------

    void on_stock_directory(const itch::StockDirectory& m) {
        ensure_locate(m.h.locate);
        symbols_[m.h.locate] = std::string(itch::trim_stock(m.stock));
    }

    void on_add(const itch::AddOrder& m) {
        ++stats_.adds;
        add_order(m.h.locate, m.order_ref, m.side, m.shares, m.price);
    }

    void on_executed(const itch::OrderExecuted& m) {
        ++stats_.executions;
        const std::uint32_t idx = refs_.find(m.order_ref);
        if (idx == FlatHashMap::kNotFound) { ++stats_.unknown_ref; return; }
        const Order& o = pool_[idx];
        record_trade(o.locate, o.price, m.executed_shares, true);
        reduce(idx, m.order_ref, m.executed_shares);
    }

    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        ++stats_.executions;
        const std::uint32_t idx = refs_.find(m.order_ref);
        if (idx == FlatHashMap::kNotFound) { ++stats_.unknown_ref; return; }
        record_trade(pool_[idx].locate, m.execution_price, m.executed_shares, m.printable == 'Y');
        reduce(idx, m.order_ref, m.executed_shares);
    }

    void on_cancel(const itch::OrderCancel& m) {
        ++stats_.cancels;
        const std::uint32_t idx = refs_.find(m.order_ref);
        if (idx == FlatHashMap::kNotFound) { ++stats_.unknown_ref; return; }
        reduce(idx, m.order_ref, m.cancelled_shares);
    }

    void on_delete(const itch::OrderDelete& m) {
        ++stats_.deletes;
        const std::uint32_t idx = refs_.erase(m.order_ref);
        if (idx == FlatHashMap::kNotFound) { ++stats_.unknown_ref; return; }
        unlink(idx);
    }

    void on_replace(const itch::OrderReplace& m) {
        ++stats_.replaces;
        const std::uint32_t idx = refs_.erase(m.original_ref);
        if (idx == FlatHashMap::kNotFound) { ++stats_.unknown_ref; return; }
        const Order old = pool_[idx];  // copy before the slot is recycled
        unlink(idx);
        add_order(old.locate, m.new_ref, old.side, m.shares, m.price);
    }

    void on_trade(const itch::Trade& m) {
        ensure_locate(m.h.locate);
        record_trade(m.h.locate, m.price, m.shares, true);
    }

    void on_cross_trade(const itch::CrossTrade& m) {
        ensure_locate(m.h.locate);
        if (m.shares > 0)
            record_trade(m.h.locate, m.cross_price, static_cast<std::uint32_t>(m.shares), true);
    }

    // ---- queries ------------------------------------------------------------

    const Book* book(std::uint16_t locate) const noexcept {
        return locate < books_.size() ? &books_[locate] : nullptr;
    }

    // Linear search: fine for setup-time lookups, never on the hot path.
    int locate_of(std::string_view symbol) const noexcept {
        for (std::size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == symbol) return static_cast<int>(i);
        return -1;
    }

    const std::string& symbol(std::uint16_t locate) const { return symbols_[locate]; }
    std::size_t num_locates() const noexcept { return books_.size(); }

    const Order* find_order(std::uint64_t ref) const noexcept {
        const std::uint32_t idx = refs_.find(ref);
        return idx == FlatHashMap::kNotFound ? nullptr : &pool_[idx];
    }

    // 0-based position of an order in its price level's queue (0 = next to fill).
    // O(queue length); for analysis, not the hot path.
    std::size_t queue_position(std::uint64_t ref) const {
        const std::uint32_t idx = refs_.find(ref);
        if (idx == FlatHashMap::kNotFound) return static_cast<std::size_t>(-1);
        std::size_t pos = 0;
        for (std::uint32_t i = pool_[idx].prev; i != kNil; i = pool_[i].prev) ++pos;
        return pos;
    }

    const OrderPool& pool() const noexcept { return pool_; }
    const BuilderStats& stats() const noexcept { return stats_; }
    std::size_t live_orders() const noexcept { return refs_.size(); }

    // Walks every level's FIFO and checks it agrees with the level totals.
    // Expensive: for tests and debugging only.
    bool check_invariants(std::string* why = nullptr) const {
        for (std::size_t loc = 0; loc < books_.size(); ++loc) {
            const Book& b = books_[loc];
            if (!check_side(b.bids, Side::Buy, loc, why)) return false;
            if (!check_side(b.asks, Side::Sell, loc, why)) return false;
        }
        return true;
    }

private:
    void ensure_locate(std::uint16_t locate) {
        if (locate >= books_.size()) [[unlikely]] {
            books_.resize(std::size_t{locate} + 1);
            symbols_.resize(std::size_t{locate} + 1);
        }
    }

    void add_order(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
                   Price price) {
        ensure_locate(locate);
        const std::uint32_t idx = pool_.alloc();
        if (!refs_.insert(ref, idx)) [[unlikely]] {
            // The ref is already live (bad input). Drop the old order so the
            // book stays consistent, then insert the new one.
            ++stats_.duplicate_ref;
            unlink(refs_.erase(ref));
            refs_.insert(ref, idx);
        }
        Order& o = pool_[idx];
        o = Order{ref, price, shares, kNil, kNil, locate, side};

        Book& b = books_[locate];
        if (side == Side::Buy) {
            append(b.bids.find_or_insert(price), idx);
            if (b.asks.best() && price >= b.asks.best()->price) ++stats_.crossed_after_add;
        } else {
            append(b.asks.find_or_insert(price), idx);
            if (b.bids.best() && price <= b.bids.best()->price) ++stats_.crossed_after_add;
        }
    }

    // Push order `idx` onto the back of a level's FIFO.
    void append(Level& lv, std::uint32_t idx) noexcept {
        Order& o = pool_[idx];
        o.prev = lv.tail;
        o.next = kNil;
        if (lv.tail != kNil) pool_[lv.tail].next = idx;
        else lv.head = idx;
        lv.tail = idx;
        ++lv.count;
        lv.qty += o.shares;
    }

    // Remove `shares` from an order; deletes it when nothing is left.
    void reduce(std::uint32_t idx, std::uint64_t ref, std::uint32_t shares) {
        Order& o = pool_[idx];
        if (shares >= o.shares) {
            if (shares > o.shares) ++stats_.overfill;
            refs_.erase(ref);
            unlink(idx);
            return;
        }
        o.shares -= shares;
        Level* lv = find_level(o);
        lv->qty -= shares;
    }

    // Take an order out of its level (erasing the level if it empties) and
    // return its slot to the pool. Does NOT touch the ref map.
    void unlink(std::uint32_t idx) {
        Order& o = pool_[idx];
        Book& b = books_[o.locate];
        Level* lv = find_level(o);

        if (o.prev != kNil) pool_[o.prev].next = o.next;
        else lv->head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev;
        else lv->tail = o.prev;

        lv->qty -= o.shares;
        if (--lv->count == 0) {
            if (o.side == Side::Buy) b.bids.erase(o.price);
            else b.asks.erase(o.price);
        }
        pool_.release(idx);
    }

    Level* find_level(const Order& o) noexcept {
        Book& b = books_[o.locate];
        return o.side == Side::Buy ? b.bids.find(o.price) : b.asks.find(o.price);
    }

    void record_trade(std::uint16_t locate, Price price, std::uint32_t shares, bool printable) {
        Book& b = books_[locate];
        ++b.trades;
        b.last_price = price;
        b.last_shares = shares;
        if (printable) b.volume += shares;
    }

    template <typename Levels>
    bool check_side(const Levels& side, Side s, std::size_t loc, std::string* why) const {
        bool ok = true;
        bool first = true;
        Price prev_price = 0;
        side.for_each(~std::size_t{0}, [&](const Level& lv) {
            if (!ok) return;
            auto fail = [&](const std::string& msg) {
                ok = false;
                if (why) *why = "locate " + std::to_string(loc) + " price " +
                                std::to_string(lv.price) + ": " + msg;
            };
            if (!first) {
                const bool ordered = s == Side::Buy ? lv.price < prev_price : lv.price > prev_price;
                if (!ordered) return fail("levels out of order");
            }
            first = false;
            prev_price = lv.price;
            if (lv.count == 0 || lv.qty == 0) return fail("empty level still in book");

            std::uint64_t qty = 0;
            std::uint32_t count = 0;
            std::uint32_t prev = kNil;
            for (std::uint32_t i = lv.head; i != kNil; i = pool_[i].next) {
                const Order& o = pool_[i];
                if (o.prev != prev) return fail("broken prev link");
                if (o.price != lv.price || o.side != s || o.locate != loc)
                    return fail("order in wrong level");
                if (refs_.find(o.ref) != i) return fail("ref map disagrees with FIFO");
                qty += o.shares;
                ++count;
                prev = i;
            }
            if (prev != lv.tail) return fail("tail mismatch");
            if (qty != lv.qty || count != lv.count) return fail("totals mismatch");
        });
        return ok;
    }

    OrderPool pool_;
    FlatHashMap refs_;            // order ref -> pool index
    std::vector<Book> books_;     // indexed by stock locate
    std::vector<std::string> symbols_;
    BuilderStats stats_;
};

}  // namespace hft::book
