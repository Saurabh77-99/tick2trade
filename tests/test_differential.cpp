// Differential test: the optimized books must agree with a deliberately naive
// reference implementation after EVERY message of a synthetic trading day.
//
// The reference uses std::map and std::unordered_map with no cleverness at
// all, so it's easy to convince yourself it's correct. If the fast
// implementations ever disagree with it, the test prints the exact message
// index, which makes the bug easy to reproduce.

#include <gtest/gtest.h>

#include <map>
#include <unordered_map>
#include <vector>

#include "hft/book/book_builder.hpp"
#include "hft/itch/parser.hpp"
#include "hft/sim/synthetic_feed.hpp"

using namespace hft;
using itch::Price;
using itch::Side;

namespace {

struct NaiveBook : itch::NullHandler {
    struct O {
        std::uint16_t loc;
        Side side;
        Price price;
        std::uint32_t shares;
    };
    std::unordered_map<std::uint64_t, O> orders;
    std::map<std::uint16_t, std::map<Price, std::uint64_t>> bids, asks;

    void on_add(const itch::AddOrder& m) { add(m.h.locate, m.order_ref, m.side, m.shares, m.price); }
    void on_executed(const itch::OrderExecuted& m) { reduce(m.order_ref, m.executed_shares); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { reduce(m.order_ref, m.executed_shares); }
    void on_cancel(const itch::OrderCancel& m) { reduce(m.order_ref, m.cancelled_shares); }
    void on_delete(const itch::OrderDelete& m) { reduce(m.order_ref, orders.at(m.order_ref).shares); }
    void on_replace(const itch::OrderReplace& m) {
        const O o = orders.at(m.original_ref);
        reduce(m.original_ref, o.shares);
        add(o.loc, m.new_ref, o.side, m.shares, m.price);
    }

    void add(std::uint16_t loc, std::uint64_t ref, Side s, std::uint32_t shares, Price p) {
        orders[ref] = {loc, s, p, shares};
        (s == Side::Buy ? bids : asks)[loc][p] += shares;
    }

    void reduce(std::uint64_t ref, std::uint32_t n) {
        O& o = orders.at(ref);
        auto& lv = (o.side == Side::Buy ? bids : asks)[o.loc];
        lv[o.price] -= n;
        if (lv[o.price] == 0) lv.erase(o.price);
        o.shares -= n;
        if (o.shares == 0) orders.erase(ref);
    }

    std::vector<std::pair<Price, std::uint64_t>> top(std::uint16_t loc, Side s, std::size_t n) {
        std::vector<std::pair<Price, std::uint64_t>> out;
        if (s == Side::Buy) {
            auto& m = bids[loc];
            for (auto it = m.rbegin(); it != m.rend() && out.size() < n; ++it) out.emplace_back(it->first, it->second);
        } else {
            auto& m = asks[loc];
            for (auto it = m.begin(); it != m.end() && out.size() < n; ++it) out.emplace_back(it->first, it->second);
        }
        return out;
    }
};

template <typename Builder>
std::vector<std::pair<Price, std::uint64_t>> top(const Builder& b, std::uint16_t loc, Side s, std::size_t n) {
    std::vector<std::pair<Price, std::uint64_t>> out;
    const auto* book = b.book(loc);
    if (!book) return out;
    auto f = [&](const book::Level& lv) { out.emplace_back(lv.price, lv.qty); };
    if (s == Side::Buy) book->bids.for_each(n, f);
    else book->asks.for_each(n, f);
    return out;
}

void run_differential(std::uint64_t seed, std::uint64_t messages, int symbols) {
    sim::FeedConfig cfg;
    cfg.seed = seed;
    cfg.messages = messages;
    cfg.symbols = symbols;
    itch::Writer w;
    sim::SyntheticFeed(cfg).generate(w);

    book::BookBuilder<book::VectorPolicy> vec(1 << 12);
    book::BookBuilder<book::MapPolicy> map(1 << 12);
    NaiveBook naive;

    const auto& buf = w.bytes();
    std::size_t pos = 0, index = 0;
    while (pos + 2 <= buf.size()) {
        const std::uint16_t len = load_be16(buf.data() + pos);
        const std::uint8_t* msg = buf.data() + pos + 2;
        pos += 2 + std::size_t{len};
        ++index;

        itch::dispatch(msg, len, vec);
        itch::dispatch(msg, len, map);
        itch::dispatch(msg, len, naive);

        const std::uint16_t loc = load_be16(msg + 1);
        for (Side s : {Side::Buy, Side::Sell}) {
            const auto expect = naive.top(loc, s, 10);
            ASSERT_EQ(top(vec, loc, s, 10), expect) << "vector book diverged at message " << index;
            ASSERT_EQ(top(map, loc, s, 10), expect) << "map book diverged at message " << index;
        }
        if (index % 25'000 == 0) {
            std::string why;
            ASSERT_TRUE(vec.check_invariants(&why)) << "message " << index << ": " << why;
            ASSERT_TRUE(map.check_invariants(&why)) << "message " << index << ": " << why;
        }
    }
    EXPECT_EQ(pos, buf.size());
    EXPECT_EQ(vec.live_orders(), naive.orders.size());
    EXPECT_EQ(map.live_orders(), naive.orders.size());

    // The generator never produces bad references or crossed books.
    const auto& st = vec.stats();
    EXPECT_EQ(st.unknown_ref, 0u);
    EXPECT_EQ(st.duplicate_ref, 0u);
    EXPECT_EQ(st.overfill, 0u);
    EXPECT_EQ(st.crossed_after_add, 0u);
    std::string why;
    EXPECT_TRUE(vec.check_invariants(&why)) << why;
}

}  // namespace

TEST(Differential, OneBusySymbol) { run_differential(1, 150'000, 1); }
TEST(Differential, ManySymbols) { run_differential(2, 300'000, 12); }
TEST(Differential, AnotherSeed) { run_differential(99, 200'000, 5); }
