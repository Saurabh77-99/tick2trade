#pragma once
// A deliberately naive matching engine, used as the test oracle.
//
// All resting orders live in one flat vector. To find the next order to
// trade with, it scans every order and picks the best price, then the
// lowest arrival sequence number. O(n) per step and obviously correct,
// which is the point: the real engine must produce exactly the same
// reports and the same book after every request.
//
// Both engines report through the same EventLog, which turns every report
// into one line of text, so a mismatch prints something readable.

#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#include "hft/exchange/types.hpp"

namespace hft::test {

using namespace hft::exchange;

inline char side_char(Side s) { return s == Side::Buy ? 'B' : 'S'; }

struct EventLog : NullListener {
    std::vector<std::string> lines;

    void on_accepted(const Accepted& e) {
        add("ACK t=%llu tr=%u cid=%llu id=%llu loc=%u %c px=%u q=%u", e.ts, e.trader, e.client_order_id,
            e.order_id, e.locate, side_char(e.side), e.price, e.qty);
    }
    void on_rejected(const Rejected& e) {
        add("REJ t=%llu tr=%u cid=%llu id=%llu %s", e.ts, e.trader, e.client_order_id, e.order_id,
            to_string(e.reason));
    }
    void on_fill(const Fill& e) {
        add("FILL t=%llu tr=%u id=%llu cid=%llu loc=%u %c px=%u q=%u leaves=%u match=%llu %s", e.ts, e.trader,
            e.order_id, e.client_order_id, e.locate, side_char(e.side), e.price, e.qty, e.leaves, e.match_id,
            e.maker ? "maker" : "taker");
    }
    void on_canceled(const Canceled& e) {
        add("CXL t=%llu tr=%u id=%llu cid=%llu loc=%u q=%u %s", e.ts, e.trader, e.order_id, e.client_order_id,
            e.locate, e.qty, to_string(e.reason));
    }
    void on_replaced(const Replaced& e) {
        add("RPL t=%llu tr=%u old=%llu new=%llu cid=%llu loc=%u px=%u q=%u", e.ts, e.trader, e.old_order_id,
            e.new_order_id, e.client_order_id, e.locate, e.price, e.qty);
    }

private:
    template <typename... A>
    void add(const char* fmt, A... a) {
        char buf[256];
        std::snprintf(buf, sizeof buf, fmt, widen(a)...);
        lines.emplace_back(buf);
    }
    // Promote integers so the %llu / %u specifiers above are always correct.
    static unsigned long long widen(std::uint64_t v) { return v; }
    static unsigned widen(std::uint32_t v) { return v; }
    static unsigned widen(std::uint16_t v) { return v; }
    static int widen(char v) { return v; }
    static const char* widen(const char* v) { return v; }
};

class ReferenceEngine {
public:
    struct Resting {
        std::uint64_t id;
        std::uint32_t trader;
        std::uint64_t cid;
        std::uint16_t loc;
        Side side;
        Price price;
        std::uint32_t qty;
        std::uint64_t seq;  // arrival order: lower = earlier in the queue
    };

    explicit ReferenceEngine(EventLog& log, EngineConfig cfg = {}) : log_(log), cfg_(cfg) {}

    void add_symbol(std::uint16_t loc) {
        if (loc >= active_.size()) active_.resize(loc + 1u, false);
        active_[loc] = true;
    }

    void submit(const NewOrder& o, std::uint64_t ts) {
        if (o.locate >= active_.size() || !active_[o.locate]) return reject_new(o, RejectReason::UnknownSymbol, ts);
        if (o.qty == 0 || o.qty > cfg_.max_qty) return reject_new(o, RejectReason::InvalidQty, ts);
        if (o.market ? o.post_only : !valid(o.price)) return reject_new(o, RejectReason::InvalidPrice, ts);
        if (o.post_only && crosses_book(o.locate, o.side, o.price))
            return reject_new(o, RejectReason::PostOnlyWouldCross, ts);
        const std::uint64_t id = next_id_++;
        log_.on_accepted(Accepted{ts, o.trader, o.client_order_id, id, o.locate, o.side, o.market ? 0 : o.price, o.qty});
        execute(o.trader, o.client_order_id, id, o.locate, o.side, o.price, o.qty, o.tif, o.market, ts);
    }

    void cancel(const CancelOrder& c, std::uint64_t ts) {
        auto it = find(c.order_id);
        if (it == book_.end()) return reject_old(c.trader, c.order_id, RejectReason::UnknownOrder, ts);
        if (it->trader != c.trader) return reject_old(c.trader, c.order_id, RejectReason::NotOwner, ts);
        const Resting r = *it;
        book_.erase(it);
        log_.on_canceled(Canceled{ts, r.trader, r.id, r.cid, r.loc, r.qty, CancelReason::User});
    }

    void modify(const ModifyOrder& m, std::uint64_t ts) {
        auto it = find(m.order_id);
        if (it == book_.end()) return reject_old(m.trader, m.order_id, RejectReason::UnknownOrder, ts);
        if (it->trader != m.trader) return reject_old(m.trader, m.order_id, RejectReason::NotOwner, ts);
        if (m.new_qty == 0 || m.new_qty > cfg_.max_qty) return reject_old(m.trader, m.order_id, RejectReason::InvalidQty, ts);
        if (!valid(m.new_price)) return reject_old(m.trader, m.order_id, RejectReason::InvalidPrice, ts);

        if (m.new_price == it->price && m.new_qty <= it->qty) {
            it->qty = m.new_qty;  // keeps its seq: same place in line
            log_.on_replaced(Replaced{ts, it->trader, it->id, it->id, it->cid, it->loc, it->price, it->qty});
            return;
        }
        const std::uint64_t new_id = next_id_++;
        if (!crosses_book(it->loc, it->side, m.new_price)) {
            const std::uint64_t old_id = it->id;
            it->id = new_id;
            it->price = m.new_price;
            it->qty = m.new_qty;
            it->seq = seq_++;  // back of the line
            log_.on_replaced(Replaced{ts, it->trader, old_id, new_id, it->cid, it->loc, it->price, it->qty});
            return;
        }
        const Resting r = *it;
        book_.erase(it);
        log_.on_replaced(Replaced{ts, r.trader, r.id, new_id, r.cid, r.loc, m.new_price, m.new_qty});
        execute(r.trader, r.cid, new_id, r.loc, r.side, m.new_price, m.new_qty, TimeInForce::Day, false, ts);
    }

    // Resting orders for one side in priority order: (id, price, qty, trader).
    std::vector<std::tuple<std::uint64_t, Price, std::uint32_t, std::uint32_t>> side(std::uint16_t loc, Side s) const {
        std::vector<Resting> v;
        for (const auto& r : book_)
            if (r.loc == loc && r.side == s) v.push_back(r);
        std::sort(v.begin(), v.end(), [&](const Resting& a, const Resting& b) {
            if (a.price != b.price) return s == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.seq < b.seq;
        });
        std::vector<std::tuple<std::uint64_t, Price, std::uint32_t, std::uint32_t>> out;
        for (const auto& r : v) out.emplace_back(r.id, r.price, r.qty, r.trader);
        return out;
    }

    std::size_t live() const { return book_.size(); }

private:
    bool valid(Price p) const { return p > 0 && p % cfg_.tick == 0; }

    static bool acceptable(Side taker, bool market, Price limit, Price resting) {
        if (market) return true;
        return taker == Side::Buy ? resting <= limit : resting >= limit;
    }

    bool crosses_book(std::uint16_t loc, Side s, Price p) const {
        for (const auto& r : book_)
            if (r.loc == loc && r.side != s && acceptable(s, false, p, r.price)) return true;
        return false;
    }

    std::vector<Resting>::iterator find(std::uint64_t id) {
        return std::find_if(book_.begin(), book_.end(), [&](const Resting& r) { return r.id == id; });
    }

    // Index of the best opposite order this taker may trade with, or -1.
    long best_opposite(std::uint16_t loc, Side s, bool market, Price limit) const {
        long best = -1;
        for (std::size_t i = 0; i < book_.size(); ++i) {
            const Resting& r = book_[i];
            if (r.loc != loc || r.side == s || !acceptable(s, market, limit, r.price)) continue;
            if (best < 0) { best = long(i); continue; }
            const Resting& b = book_[std::size_t(best)];
            const bool better = r.price != b.price ? (s == Side::Buy ? r.price < b.price : r.price > b.price)
                                                   : r.seq < b.seq;
            if (better) best = long(i);
        }
        return best;
    }

    void execute(std::uint32_t trader, std::uint64_t cid, std::uint64_t id, std::uint16_t loc, Side s,
                 Price limit, std::uint32_t qty, TimeInForce tif, bool market, std::uint64_t ts) {
        std::uint32_t remaining = qty;
        if (tif == TimeInForce::FOK) {
            std::uint64_t avail = 0;
            for (const auto& r : book_)
                if (r.loc == loc && r.side != s && acceptable(s, market, limit, r.price) &&
                    !(cfg_.self_trade_prevention && r.trader == trader))
                    avail += r.qty;
            if (avail < remaining) {
                log_.on_canceled(Canceled{ts, trader, id, cid, loc, remaining, CancelReason::FokUnfilled});
                return;
            }
        }
        while (remaining > 0) {
            const long bi = best_opposite(loc, s, market, limit);
            if (bi < 0) break;
            Resting& m = book_[std::size_t(bi)];
            if (cfg_.self_trade_prevention && m.trader == trader) {
                const Resting r = m;
                book_.erase(book_.begin() + bi);
                log_.on_canceled(Canceled{ts, r.trader, r.id, r.cid, r.loc, r.qty, CancelReason::SelfTrade});
                continue;
            }
            const std::uint32_t fill = std::min(remaining, m.qty);
            m.qty -= fill;
            remaining -= fill;
            const std::uint64_t match = next_match_++;
            log_.on_fill(Fill{ts, m.trader, m.id, m.cid, loc, m.side, m.price, fill, m.qty, match, true});
            log_.on_fill(Fill{ts, trader, id, cid, loc, s, m.price, fill, remaining, match, false});
            if (m.qty == 0) book_.erase(book_.begin() + bi);
        }
        if (remaining == 0) return;
        if (market || tif != TimeInForce::Day) {
            log_.on_canceled(Canceled{ts, trader, id, cid, loc, remaining, CancelReason::IocRemainder});
            return;
        }
        book_.push_back(Resting{id, trader, cid, loc, s, limit, remaining, seq_++});
    }

    void reject_new(const NewOrder& o, RejectReason r, std::uint64_t ts) {
        log_.on_rejected(Rejected{ts, o.trader, o.client_order_id, 0, r});
    }
    void reject_old(std::uint32_t trader, std::uint64_t id, RejectReason r, std::uint64_t ts) {
        log_.on_rejected(Rejected{ts, trader, 0, id, r});
    }

    EventLog& log_;
    EngineConfig cfg_;
    std::vector<bool> active_;
    std::vector<Resting> book_;
    std::uint64_t next_id_ = 1, next_match_ = 1, seq_ = 0;
};

}  // namespace hft::test
