#pragma once
// MatchingEngine: the core of an exchange.
//
// Continuous limit order book with price-time priority:
//   * a buy at price P trades with resting sells priced <= P, best (lowest)
//     price first; within a price, oldest order first (FIFO)
//   * trades happen at the RESTING order's price: the order that was there
//     first sets the price
//   * whatever doesn't trade rests in the book (DAY), or is canceled (IOC,
//     market), or the whole order is killed if it can't fully fill (FOK)
//
// Every state change produces two kinds of output:
//   1. private reports to the traders involved (Listener callbacks)
//   2. public, anonymous market data as ITCH 5.0 messages (optional Writer)
// The Phase 1 BookBuilder rebuilds exactly this engine's book from (2); the
// tests check that after every single request.
//
// Determinism: the engine has no clock and no randomness. The caller passes
// each request's timestamp, so the same input always produces byte-identical
// output. That makes bugs reproducible, and it's what lets an exchange
// replay its journal to recover after a crash (event sourcing).
//
// Data layout reuses Phase 1's book pieces: orders in an OrderPool with
// intrusive per-level FIFOs, ids in a FlatHashMap. Order is the "hot" data
// touched while matching (32 bytes). Who owns an order and its client id
// are "cold" (only needed when reporting), so they live in a separate
// parallel array and don't take cache space during the matching loop.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hft/book/levels.hpp"
#include "hft/book/types.hpp"
#include "hft/common/flat_hash_map.hpp"
#include "hft/exchange/types.hpp"
#include "hft/itch/writer.hpp"

namespace hft::exchange {

struct EngineConfig {
    Price tick = 100;                    // $0.01 with 4 implied decimals
    std::uint32_t max_qty = 1'000'000;   // fat-finger limit per order
    bool self_trade_prevention = true;   // cancel the resting order instead of trading with yourself
};

struct EngineStats {
    std::uint64_t new_orders = 0, accepted = 0, rejected = 0;
    std::uint64_t cancels = 0, modifies = 0;
    std::uint64_t matches = 0, shares_traded = 0;
    std::uint64_t self_trade_cancels = 0;
};

template <typename Listener = NullListener, typename Policy = book::VectorPolicy>
class MatchingEngine {
public:
    struct Book {
        std::string symbol;
        bool active = false;
        typename Policy::template Levels<Side::Buy> bids;
        typename Policy::template Levels<Side::Sell> asks;
        Price last_price = 0;
        std::uint64_t volume = 0;

        Price best_bid() const noexcept { return bids.best() ? bids.best()->price : 0; }
        Price best_ask() const noexcept { return asks.best() ? asks.best()->price : 0; }
    };

    MatchingEngine(Listener& out, itch::Writer* market_data = nullptr, EngineConfig cfg = {},
                   std::size_t expected_orders = 1 << 16)
        : out_(out), md_(market_data), cfg_(cfg) {
        pool_.reserve(expected_orders);
        meta_.reserve(expected_orders);
        ids_.reserve(expected_orders);
    }

    // ---- setup ----------------------------------------------------------------

    void add_symbol(std::uint16_t locate, std::string_view name, std::uint64_t ts) {
        if (locate >= books_.size()) books_.resize(std::size_t{locate} + 1);
        books_[locate].symbol = std::string(name);
        books_[locate].active = true;
        if (md_) {
            md_->stock_directory(locate, ts, name);
            md_->trading_action(locate, ts, name, 'T');
        }
    }

    // ---- requests -------------------------------------------------------------

    void submit(const NewOrder& o, std::uint64_t ts) {
        ++stats_.new_orders;
        auto reject = [&](RejectReason r) {
            ++stats_.rejected;
            out_.on_rejected(Rejected{ts, o.trader, o.client_order_id, 0, r});
        };
        if (o.locate >= books_.size() || !books_[o.locate].active) return reject(RejectReason::UnknownSymbol);
        if (o.qty == 0 || o.qty > cfg_.max_qty) return reject(RejectReason::InvalidQty);
        if (o.market ? o.post_only : !valid_price(o.price)) return reject(RejectReason::InvalidPrice);

        Book& b = books_[o.locate];
        if (o.post_only && would_cross(b, o.side, o.price)) return reject(RejectReason::PostOnlyWouldCross);

        Taker t{o.trader, o.client_order_id, next_order_id_++, o.locate, o.side,
                o.price, o.qty, o.tif, o.market};
        ++stats_.accepted;
        out_.on_accepted(Accepted{ts, o.trader, o.client_order_id, t.order_id, o.locate, o.side,
                                  o.market ? 0 : o.price, o.qty});
        execute(b, t, ts);
    }

    void cancel(const CancelOrder& c, std::uint64_t ts) {
        ++stats_.cancels;
        const std::uint32_t idx = ids_.find(c.order_id);
        if (idx == FlatHashMap::kNotFound) return reject_existing(c.trader, c.order_id, RejectReason::UnknownOrder, ts);
        if (meta_[idx].trader != c.trader) return reject_existing(c.trader, c.order_id, RejectReason::NotOwner, ts);
        cancel_resting(idx, CancelReason::User, ts);
    }

    void modify(const ModifyOrder& m, std::uint64_t ts) {
        ++stats_.modifies;
        const std::uint32_t idx = ids_.find(m.order_id);
        if (idx == FlatHashMap::kNotFound) return reject_existing(m.trader, m.order_id, RejectReason::UnknownOrder, ts);
        if (meta_[idx].trader != m.trader) return reject_existing(m.trader, m.order_id, RejectReason::NotOwner, ts);
        if (m.new_qty == 0 || m.new_qty > cfg_.max_qty)
            return reject_existing(m.trader, m.order_id, RejectReason::InvalidQty, ts);
        if (!valid_price(m.new_price)) return reject_existing(m.trader, m.order_id, RejectReason::InvalidPrice, ts);

        book::Order& o = pool_[idx];
        const Meta meta = meta_[idx];
        Book& b = books_[o.locate];

        // Same price, smaller size: shrink in place and KEEP time priority.
        // (Every major exchange works this way: reducing your order can't
        // hurt anyone behind you in the queue.)
        if (m.new_price == o.price && m.new_qty <= o.shares) {
            const std::uint32_t delta = o.shares - m.new_qty;
            if (delta > 0) {
                level_of(b, o)->qty -= delta;
                o.shares = m.new_qty;
                if (md_) md_->order_cancel(o.locate, ts, o.ref, delta);
            }
            out_.on_replaced(Replaced{ts, meta.trader, o.ref, o.ref, meta.client_order_id, o.locate,
                                      o.price, o.shares});
            return;
        }

        // Price change or size increase: the order loses its place in line
        // and gets a new id.
        const std::uint64_t old_id = o.ref;
        const std::uint64_t new_id = next_order_id_++;
        const Side side = o.side;
        const std::uint16_t locate = o.locate;

        if (!would_cross(b, side, m.new_price)) {
            // Moves to the back of the queue at the new price: ITCH 'U'.
            detach(b, idx);
            ids_.erase(old_id);
            o.ref = new_id;
            o.price = m.new_price;
            o.shares = m.new_qty;
            ids_.insert(new_id, idx);
            append(b, idx);
            if (md_) md_->order_replace(locate, ts, old_id, new_id, m.new_qty, m.new_price);
            out_.on_replaced(Replaced{ts, meta.trader, old_id, new_id, meta.client_order_id, locate,
                                      m.new_price, m.new_qty});
            return;
        }

        // The new price crosses the spread: pull the old order (ITCH 'D') and
        // treat the change like a new aggressive order that may trade.
        remove(b, idx);
        if (md_) md_->order_delete(locate, ts, old_id);
        out_.on_replaced(Replaced{ts, meta.trader, old_id, new_id, meta.client_order_id, locate,
                                  m.new_price, m.new_qty});
        Taker t{meta.trader, meta.client_order_id, new_id, locate, side, m.new_price, m.new_qty,
                TimeInForce::Day, false};
        execute(b, t, ts);
    }

    // ---- queries ----------------------------------------------------------------

    const Book* book(std::uint16_t locate) const noexcept {
        return locate < books_.size() && books_[locate].active ? &books_[locate] : nullptr;
    }

    const book::Order* find(std::uint64_t order_id) const noexcept {
        const std::uint32_t idx = ids_.find(order_id);
        return idx == FlatHashMap::kNotFound ? nullptr : &pool_[idx];
    }

    std::uint32_t owner(std::uint64_t order_id) const noexcept {
        const std::uint32_t idx = ids_.find(order_id);
        return idx == FlatHashMap::kNotFound ? ~0u : meta_[idx].trader;
    }

    // Visit resting orders on one side, in priority order (best price, then
    // FIFO): fn(order_id, price, shares, trader).
    template <typename F>
    void for_each_resting(std::uint16_t locate, Side side, F&& fn) const {
        const Book* b = book(locate);
        if (!b) return;
        auto walk = [&](const book::Level& lv) {
            for (std::uint32_t i = lv.head; i != book::kNil; i = pool_[i].next)
                fn(pool_[i].ref, pool_[i].price, pool_[i].shares, meta_[i].trader);
        };
        if (side == Side::Buy) b->bids.for_each(~std::size_t{0}, walk);
        else b->asks.for_each(~std::size_t{0}, walk);
    }

    const EngineStats& stats() const noexcept { return stats_; }
    std::size_t live_orders() const noexcept { return ids_.size(); }
    std::size_t num_locates() const noexcept { return books_.size(); }

    // Full consistency check (tests): FIFO links, level totals, id map, sort
    // order, and the book is never crossed.
    bool check_invariants(std::string* why = nullptr) const {
        std::size_t total = 0;
        for (std::size_t loc = 0; loc < books_.size(); ++loc) {
            const Book& b = books_[loc];
            if (!b.active) continue;
            if (b.bids.best() && b.asks.best() && b.bids.best()->price >= b.asks.best()->price)
                return fail(why, "crossed book at locate " + std::to_string(loc));
            if (!check_side(b.bids, Side::Buy, loc, total, why)) return false;
            if (!check_side(b.asks, Side::Sell, loc, total, why)) return false;
        }
        if (total != ids_.size()) return fail(why, "id map size != resting orders");
        return true;
    }

private:
    struct Meta {
        std::uint32_t trader;
        std::uint64_t client_order_id;
    };

    // An incoming order while it trades.
    struct Taker {
        std::uint32_t trader;
        std::uint64_t client_order_id;
        std::uint64_t order_id;
        std::uint16_t locate;
        Side side;
        Price price;
        std::uint32_t remaining;
        TimeInForce tif;
        bool market;
    };

    bool valid_price(Price p) const noexcept { return p > 0 && p % cfg_.tick == 0; }

    // Would an order at `price` on `side` trade against the opposite best?
    static bool would_cross(const Book& b, Side side, Price price) noexcept {
        if (side == Side::Buy) return b.asks.best() && b.asks.best()->price <= price;
        return b.bids.best() && b.bids.best()->price >= price;
    }

    void execute(Book& b, Taker& t, std::uint64_t ts) {
        if (t.side == Side::Buy) match(b, b.asks, t, ts);
        else match(b, b.bids, t, ts);

        if (t.remaining == 0) return;
        if (t.market || t.tif != TimeInForce::Day) {
            // IOC / market remainder (FOK never gets here: it either fully
            // fills or is killed inside match()).
            out_.on_canceled(Canceled{ts, t.trader, t.order_id, t.client_order_id, t.locate,
                                      t.remaining, CancelReason::IocRemainder});
            return;
        }
        rest(b, t, ts);
    }

    template <typename Opposite>
    void match(Book& b, Opposite& opp, Taker& t, std::uint64_t ts) {
        auto crosses = [&](Price level_price) {
            if (t.market) return true;
            return t.side == Side::Buy ? level_price <= t.price : level_price >= t.price;
        };

        if (t.tif == TimeInForce::FOK && !fok_fillable(opp, t, crosses)) {
            out_.on_canceled(Canceled{ts, t.trader, t.order_id, t.client_order_id, t.locate,
                                      t.remaining, CancelReason::FokUnfilled});
            t.remaining = 0;
            return;
        }

        while (t.remaining > 0) {
            book::Level* lv = opp.best();
            if (!lv || !crosses(lv->price)) break;
            const Price px = lv->price;

            std::uint32_t idx = lv->head;
            while (t.remaining > 0 && idx != book::kNil) {
                book::Order& maker = pool_[idx];
                const std::uint32_t next = maker.next;
                const Meta mm = meta_[idx];

                if (cfg_.self_trade_prevention && mm.trader == t.trader) {
                    ++stats_.self_trade_cancels;
                    const std::uint32_t qty = maker.shares;
                    const std::uint64_t id = maker.ref;
                    detach_from(*lv, idx);
                    forget(idx, id);
                    if (md_) md_->order_delete(t.locate, ts, id);
                    out_.on_canceled(Canceled{ts, mm.trader, id, mm.client_order_id, t.locate, qty,
                                              CancelReason::SelfTrade});
                    idx = next;
                    continue;
                }

                const std::uint32_t fill = t.remaining < maker.shares ? t.remaining : maker.shares;
                const std::uint64_t match_id = next_match_++;
                maker.shares -= fill;
                lv->qty -= fill;
                t.remaining -= fill;
                ++stats_.matches;
                stats_.shares_traded += fill;
                b.last_price = px;
                b.volume += fill;

                // Public: the resting order was executed (ITCH reports only
                // the passive side; the aggressor was never in the book).
                if (md_) md_->order_executed(t.locate, ts, maker.ref, fill, match_id);
                out_.on_fill(Fill{ts, mm.trader, maker.ref, mm.client_order_id, t.locate, maker.side, px, fill,
                                  maker.shares, match_id, true});
                out_.on_fill(Fill{ts, t.trader, t.order_id, t.client_order_id, t.locate, t.side, px, fill,
                                  t.remaining, match_id, false});

                if (maker.shares == 0) {
                    const std::uint64_t id = maker.ref;
                    detach_from(*lv, idx);
                    forget(idx, id);
                }
                idx = next;
            }
            if (lv->count == 0) opp.erase(px);  // last use of lv
        }
    }

    // FOK pre-check: is there enough opposite liquidity at acceptable prices,
    // not counting our own orders (which self-trade prevention would cancel)?
    template <typename Opposite, typename Crosses>
    bool fok_fillable(const Opposite& opp, const Taker& t, Crosses&& crosses) const {
        std::uint64_t available = 0;
        bool done = false;
        opp.for_each(~std::size_t{0}, [&](const book::Level& lv) {
            if (done || !crosses(lv.price)) {
                done = true;
                return;
            }
            for (std::uint32_t i = lv.head; i != book::kNil && available < t.remaining; i = pool_[i].next)
                if (!cfg_.self_trade_prevention || meta_[i].trader != t.trader) available += pool_[i].shares;
            if (available >= t.remaining) done = true;
        });
        return available >= t.remaining;
    }

    void rest(Book& b, const Taker& t, std::uint64_t ts) {
        const std::uint32_t idx = pool_.alloc();
        if (idx >= meta_.size()) meta_.resize(std::size_t{idx} + 1);
        pool_[idx] = book::Order{t.order_id, t.price, t.remaining, book::kNil, book::kNil, t.locate, t.side};
        meta_[idx] = Meta{t.trader, t.client_order_id};
        ids_.insert(t.order_id, idx);
        append(b, idx);
        if (md_) md_->add_order(t.locate, ts, t.order_id, t.side, t.remaining, b.symbol, t.price);
    }

    void cancel_resting(std::uint32_t idx, CancelReason why, std::uint64_t ts) {
        book::Order& o = pool_[idx];
        const Meta mm = meta_[idx];
        const std::uint64_t id = o.ref;
        const std::uint32_t qty = o.shares;
        const std::uint16_t locate = o.locate;
        remove(books_[locate], idx);
        if (md_) md_->order_delete(locate, ts, id);
        out_.on_canceled(Canceled{ts, mm.trader, id, mm.client_order_id, locate, qty, why});
    }

    void reject_existing(std::uint32_t trader, std::uint64_t order_id, RejectReason r, std::uint64_t ts) {
        ++stats_.rejected;
        out_.on_rejected(Rejected{ts, trader, 0, order_id, r});
    }

    // ---- book plumbing --------------------------------------------------------

    book::Level* level_of(Book& b, const book::Order& o) noexcept {
        return o.side == Side::Buy ? b.bids.find(o.price) : b.asks.find(o.price);
    }

    void append(Book& b, std::uint32_t idx) {
        book::Order& o = pool_[idx];
        book::Level& lv = o.side == Side::Buy ? b.bids.find_or_insert(o.price) : b.asks.find_or_insert(o.price);
        o.prev = lv.tail;
        o.next = book::kNil;
        if (lv.tail != book::kNil) pool_[lv.tail].next = idx;
        else lv.head = idx;
        lv.tail = idx;
        ++lv.count;
        lv.qty += o.shares;
    }

    // Unlink from the level's FIFO and totals. Does not erase the level.
    void detach_from(book::Level& lv, std::uint32_t idx) noexcept {
        book::Order& o = pool_[idx];
        if (o.prev != book::kNil) pool_[o.prev].next = o.next;
        else lv.head = o.next;
        if (o.next != book::kNil) pool_[o.next].prev = o.prev;
        else lv.tail = o.prev;
        lv.qty -= o.shares;
        --lv.count;
    }

    // Detach and erase the level if it emptied.
    void detach(Book& b, std::uint32_t idx) {
        const book::Order& o = pool_[idx];
        book::Level* lv = level_of(b, o);
        detach_from(*lv, idx);
        if (lv->count == 0) {
            if (o.side == Side::Buy) b.bids.erase(o.price);
            else b.asks.erase(o.price);
        }
    }

    // Drop the id mapping and recycle the slot.
    void forget(std::uint32_t idx, std::uint64_t id) {
        ids_.erase(id);
        pool_.release(idx);
    }

    void remove(Book& b, std::uint32_t idx) {
        const std::uint64_t id = pool_[idx].ref;
        detach(b, idx);
        forget(idx, id);
    }

    static bool fail(std::string* why, const std::string& msg) {
        if (why) *why = msg;
        return false;
    }

    template <typename Levels>
    bool check_side(const Levels& side, Side s, std::size_t loc, std::size_t& total, std::string* why) const {
        bool ok = true, first = true;
        Price prev_price = 0;
        side.for_each(~std::size_t{0}, [&](const book::Level& lv) {
            if (!ok) return;
            const std::string where = "locate " + std::to_string(loc) + " price " + std::to_string(lv.price) + ": ";
            if (!first && (s == Side::Buy ? lv.price >= prev_price : lv.price <= prev_price))
                ok = fail(why, where + "levels out of order");
            first = false;
            prev_price = lv.price;
            if (lv.count == 0 || lv.qty == 0) ok = ok && fail(why, where + "empty level");
            std::uint64_t qty = 0;
            std::uint32_t count = 0, prev = book::kNil;
            for (std::uint32_t i = lv.head; ok && i != book::kNil; i = pool_[i].next) {
                const book::Order& o = pool_[i];
                if (o.prev != prev) ok = fail(why, where + "broken prev link");
                else if (o.price != lv.price || o.side != s || o.locate != loc) ok = fail(why, where + "misplaced order");
                else if (o.shares == 0) ok = fail(why, where + "zero-share order");
                else if (ids_.find(o.ref) != i) ok = fail(why, where + "id map disagrees");
                qty += o.shares;
                ++count;
                prev = i;
            }
            if (ok && (prev != lv.tail || qty != lv.qty || count != lv.count)) ok = fail(why, where + "totals mismatch");
            total += count;
        });
        return ok;
    }

    Listener& out_;
    itch::Writer* md_;
    EngineConfig cfg_;
    book::OrderPool pool_;
    std::vector<Meta> meta_;        // cold data, parallel to pool_
    FlatHashMap ids_;               // exchange order id -> pool index
    std::vector<Book> books_;       // by stock locate
    std::uint64_t next_order_id_ = 1;
    std::uint64_t next_match_ = 1;
    EngineStats stats_;
};

}  // namespace hft::exchange
