#pragma once
// Synthetic ITCH 5.0 day generator.
//
// Produces a valid, internally consistent BinaryFILE stream: a stock
// directory, then a trading day of adds, cancels, replaces and executions
// around a random-walking "fair" price for each symbol. It is a toy model of
// a market, not a realistic one, but it obeys every rule the book builder
// relies on:
//   * every exec/cancel/delete/replace refers to a live order
//   * executions fill the oldest order at the best price (price-time priority)
//   * resting orders never cross (bid < ask at all times)
//   * timestamps never go backwards
//
// That makes it a good test oracle: replaying it must give zero errors.
//
// The generator keeps its own simple book (std::map of price -> FIFO of
// refs), written as plainly as possible and independent of src/book.

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/itch/writer.hpp"
#include "hft/sim/rng.hpp"

namespace hft::sim {

struct FeedConfig {
    int symbols = 8;
    std::uint64_t messages = 2'000'000;  // approximate (book activity only)
    std::uint64_t seed = 42;
    int target_orders_per_symbol = 300;  // steady-state resting orders
};

class SyntheticFeed {
public:
    static constexpr std::uint64_t kSecond = 1'000'000'000ull;
    static constexpr std::uint64_t kHour = 3600 * kSecond;
    static constexpr std::uint64_t kOpen = 9 * kHour + 30 * 60 * kSecond;  // 09:30
    static constexpr std::uint64_t kClose = 16 * kHour;                    // 16:00
    static constexpr itch::Price kTick = 100;                              // $0.01

    explicit SyntheticFeed(FeedConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

    void generate(itch::Writer& w) {
        setup(w);
        const double mean_gap = double(kClose - kOpen) / double(std::max<std::uint64_t>(cfg_.messages, 1));
        while (emitted_ < cfg_.messages) {
            ts_ += 1 + static_cast<std::uint64_t>(rng_.exponential(mean_gap));
            if (ts_ >= kClose - kSecond) ts_ = kClose - kSecond;  // squeeze the tail
            step(w, pick_symbol());
        }
        w.system_event(kClose, 'M');           // end of market hours
        w.system_event(20 * kHour, 'E');       // end of system hours
        w.system_event(20 * kHour + 5 * 60 * kSecond, 'C');  // end of messages
    }

    // Symbol names in locate order (locate = index + 1).
    const std::vector<std::string>& symbols() const noexcept { return names_; }

private:
    struct GenOrder {
        std::uint16_t locate;
        itch::Side side;
        itch::Price price;
        std::uint32_t shares;
        std::uint32_t live_pos;  // index into Sym::live
    };

    using Fifo = std::vector<std::uint64_t>;
    struct Sym {
        std::string name;
        std::uint16_t locate;
        std::int64_t fair;                                  // in ticks
        std::map<itch::Price, Fifo, std::greater<>> bids;   // best first
        std::map<itch::Price, Fifo, std::less<>> asks;      // best first
        std::vector<std::uint64_t> live;                    // for random picks
        double weight;                                      // activity share
    };

    void setup(itch::Writer& w) {
        static constexpr std::array<const char*, 24> kNames = {
            "AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AVGO",
            "COST", "NFLX", "AMD",  "ADBE", "PEP",   "CSCO", "INTC", "QCOM",
            "TXN",  "AMAT", "INTU", "SBUX", "MU",    "LRCX", "ADI",  "KLAC"};

        ts_ = 3 * kHour;
        w.system_event(ts_, 'O');  // start of messages
        ts_ = 4 * kHour;
        w.system_event(ts_, 'S');  // start of system hours

        const int n = std::clamp(cfg_.symbols, 1, 4000);
        double total_weight = 0;
        for (int i = 0; i < n; ++i) {
            Sym s;
            s.name = i < int(kNames.size()) ? kNames[i] : "SYM" + std::to_string(i);
            s.locate = static_cast<std::uint16_t>(i + 1);
            s.fair = 2000 + static_cast<std::int64_t>(rng_.below(40000));  // $20 - $420
            s.weight = 1.0 / (1.0 + 0.5 * i);  // a few busy names, a long tail
            total_weight += s.weight;
            names_.push_back(s.name);
            syms_.push_back(std::move(s));
        }
        double acc = 0;
        for (auto& s : syms_) {
            acc += s.weight / total_weight;
            cdf_.push_back(acc);
        }

        for (auto& s : syms_) {
            ts_ += 1000;
            w.stock_directory(s.locate, ts_, s.name);
            w.trading_action(s.locate, ts_, s.name, 'T');
        }

        // Opening liquidity: a few orders on each of the first 10 levels.
        ts_ = kOpen - 60 * kSecond;
        for (auto& s : syms_) {
            for (int lvl = 0; lvl < 10; ++lvl) {
                for (std::uint64_t k = 0, m = 1 + rng_.below(3); k < m; ++k) {
                    ts_ += 100;
                    add(w, s, itch::Side::Buy, price_of(s.fair - 1 - lvl), lot());
                    add(w, s, itch::Side::Sell, price_of(s.fair + 1 + lvl), lot());
                }
            }
        }
        ts_ = kOpen;
        w.system_event(ts_, 'Q');  // start of market hours
        emitted_ = 0;
    }

    Sym& pick_symbol() {
        const double u = rng_.unit();
        const auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return syms_[std::min<std::size_t>(it - cdf_.begin(), syms_.size() - 1)];
    }

    void step(itch::Writer& w, Sym& s) {
        // Cancel pressure rises with book size, so the book stays near target.
        const double fill = double(s.live.size()) / cfg_.target_orders_per_symbol;
        const double w_add = 45, w_cancel = 30 * fill, w_replace = 8, w_exec = 12,
                     w_hidden = 3, w_move = 2;
        double r = rng_.unit() * (w_add + w_cancel + w_replace + w_exec + w_hidden + w_move);

        if ((r -= w_add) < 0) return add_passive(w, s);
        if ((r -= w_cancel) < 0) return cancel(w, s);
        if ((r -= w_replace) < 0) return replace(w, s);
        if ((r -= w_exec) < 0) return execute(w, s);
        if ((r -= w_hidden) < 0) return hidden_trade(w, s);
        s.fair += rng_.chance(0.5) ? 1 : -1;  // fair value drifts a tick
        if (s.fair < 100) s.fair = 100;       // keep prices above $1
    }

    // ---- event types -----------------------------------------------------

    void add_passive(itch::Writer& w, Sym& s) {
        const auto side = rng_.chance(0.5) ? itch::Side::Buy : itch::Side::Sell;
        const std::int64_t offset = rng_.geometric(0.35);  // mostly near the top
        std::int64_t ticks = side == itch::Side::Buy ? s.fair - 1 - offset : s.fair + 1 + offset;
        add(w, s, side, clamp_passive(s, side, ticks), lot());
    }

    void cancel(itch::Writer& w, Sym& s) {
        if (s.live.empty()) return add_passive(w, s);
        const std::uint64_t ref = s.live[rng_.below(s.live.size())];
        GenOrder& o = orders_.at(ref);
        if (o.shares > 100 && rng_.chance(0.2)) {
            const std::uint32_t cut = 100 * static_cast<std::uint32_t>(1 + rng_.below((o.shares - 1) / 100));
            w.order_cancel(s.locate, ts_, ref, cut);
            o.shares -= cut;
        } else {
            w.order_delete(s.locate, ts_, ref);
            remove(s, ref);
        }
        ++emitted_;
    }

    void replace(itch::Writer& w, Sym& s) {
        if (s.live.empty()) return add_passive(w, s);
        const std::uint64_t old_ref = s.live[rng_.below(s.live.size())];
        const GenOrder o = orders_.at(old_ref);
        const std::int64_t move = static_cast<std::int64_t>(rng_.below(5)) - 2;  // -2..+2 ticks
        const itch::Price price =
            clamp_passive(s, o.side, static_cast<std::int64_t>(o.price / kTick) + move);
        const std::uint64_t new_ref = next_ref();
        const std::uint32_t shares = lot();
        w.order_replace(s.locate, ts_, old_ref, new_ref, shares, price);
        remove(s, old_ref);
        insert(s, new_ref, o.side, price, shares);
        ++emitted_;
    }

    // An aggressive order sweeps the oldest orders at the best opposite price.
    void execute(itch::Writer& w, Sym& s) {
        // Lean toward the side that moves the book back to fair value.
        const double mid = mid_ticks(s);
        const double p_buy = mid > 0 ? std::clamp(0.5 + 0.15 * (double(s.fair) - mid), 0.2, 0.8) : 0.5;
        const bool buy = rng_.chance(p_buy);
        auto run = [&](auto& book) {
            if (book.empty()) return add_passive(w, s);
            std::uint32_t want = lot() * static_cast<std::uint32_t>(1 + rng_.geometric(0.6));
            for (int fills = 0; want > 0 && !book.empty() && fills < 4; ++fills) {
                auto& [price, fifo] = *book.begin();
                const std::uint64_t ref = fifo.front();
                GenOrder& o = orders_.at(ref);
                const std::uint32_t qty = std::min(want, o.shares);
                const itch::Price px = price;  // copy: `price` dies if the level is erased
                if (rng_.chance(0.05))
                    w.order_executed_with_price(s.locate, ts_, ref, qty, ++match_, px, 'Y');
                else
                    w.order_executed(s.locate, ts_, ref, qty, ++match_);
                ++emitted_;
                want -= qty;
                if (qty == o.shares) remove(s, ref);
                else o.shares -= qty;
            }
        };
        if (buy) run(s.asks);
        else run(s.bids);
    }

    void hidden_trade(itch::Writer& w, Sym& s) {
        const double mid = mid_ticks(s);
        if (mid <= 0) return add_passive(w, s);
        const auto price = price_of(static_cast<std::int64_t>(mid));
        const auto side = rng_.chance(0.5) ? itch::Side::Buy : itch::Side::Sell;
        w.trade(s.locate, ts_, side, lot(), s.name, price, ++match_);
        ++emitted_;
    }

    // ---- book bookkeeping -------------------------------------------------

    void add(itch::Writer& w, Sym& s, itch::Side side, itch::Price price, std::uint32_t shares) {
        const std::uint64_t ref = next_ref();
        if (rng_.chance(0.1)) w.add_order(s.locate, ts_, ref, side, shares, s.name, price, "MPID");
        else w.add_order(s.locate, ts_, ref, side, shares, s.name, price);
        insert(s, ref, side, price, shares);
        ++emitted_;
    }

    void insert(Sym& s, std::uint64_t ref, itch::Side side, itch::Price price, std::uint32_t shares) {
        if (side == itch::Side::Buy) s.bids[price].push_back(ref);
        else s.asks[price].push_back(ref);
        orders_[ref] = GenOrder{s.locate, side, price, shares, static_cast<std::uint32_t>(s.live.size())};
        s.live.push_back(ref);
    }

    void remove(Sym& s, std::uint64_t ref) {
        const GenOrder o = orders_.at(ref);
        auto drop = [&](auto& book) {
            auto it = book.find(o.price);
            Fifo& f = it->second;
            f.erase(std::find(f.begin(), f.end(), ref));
            if (f.empty()) book.erase(it);
        };
        if (o.side == itch::Side::Buy) drop(s.bids);
        else drop(s.asks);
        // O(1) removal from the live list: swap with the last element.
        const std::uint64_t last = s.live.back();
        s.live[o.live_pos] = last;
        orders_.at(last).live_pos = o.live_pos;
        s.live.pop_back();
        orders_.erase(ref);
    }

    // Keep resting orders from crossing the opposite side.
    itch::Price clamp_passive(const Sym& s, itch::Side side, std::int64_t ticks) const {
        if (side == itch::Side::Buy) {
            if (!s.asks.empty()) ticks = std::min<std::int64_t>(ticks, s.asks.begin()->first / kTick - 1);
        } else {
            if (!s.bids.empty()) ticks = std::max<std::int64_t>(ticks, s.bids.begin()->first / kTick + 1);
        }
        return price_of(ticks);
    }

    double mid_ticks(const Sym& s) const {
        if (s.bids.empty() || s.asks.empty()) return 0;
        return (double(s.bids.begin()->first) + double(s.asks.begin()->first)) / 2.0 / kTick;
    }

    static itch::Price price_of(std::int64_t ticks) {
        return static_cast<itch::Price>(std::max<std::int64_t>(ticks, 1) * kTick);
    }

    std::uint32_t lot() {
        if (rng_.chance(0.08)) return 1 + static_cast<std::uint32_t>(rng_.below(99));  // odd lot
        return 100 * static_cast<std::uint32_t>(1 + rng_.geometric(0.45));
    }

    // Real refs are unique and increasing but not dense; mimic that.
    std::uint64_t next_ref() { return ref_ += 1 + rng_.below(4); }

    FeedConfig cfg_;
    Rng rng_;
    std::vector<Sym> syms_;
    std::vector<std::string> names_;
    std::vector<double> cdf_;
    std::unordered_map<std::uint64_t, GenOrder> orders_;
    std::uint64_t ts_ = 0;
    std::uint64_t ref_ = 1000;
    std::uint64_t match_ = 0;
    std::uint64_t emitted_ = 0;
};

}  // namespace hft::sim
