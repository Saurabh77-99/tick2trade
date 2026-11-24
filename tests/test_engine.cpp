// Matching engine tests:
//   1. scenarios: hand-written cases for each rule, readable as a spec
//   2. differential fuzz: random requests through the real engine and the
//      naive reference engine; every report and the whole book must match
//   3. feed consistency: the engine's ITCH output, rebuilt by Phase 1's
//      BookBuilder, must equal the engine's own book after every request

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include <set>

#include "hft/book/book_builder.hpp"
#include "hft/exchange/matching_engine.hpp"
#include "hft/itch/parser.hpp"
#include "hft/sim/rng.hpp"
#include "reference_engine.hpp"

using namespace hft;
using namespace hft::exchange;
using hft::test::EventLog;
using hft::test::ReferenceEngine;

namespace {

constexpr std::uint16_t L = 1;
constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10000 + 0.5); }

template <typename Policy>
class EngineTest : public ::testing::Test {
protected:
    EventLog log;
    MatchingEngine<EventLog, Policy> eng{log};
    std::uint64_t ts = 1;
    std::uint64_t cid = 1;

    EngineTest() { eng.add_symbol(L, "TEST", 0); }

    // Returns the exchange order id (0 if rejected).
    std::uint64_t limit(std::uint32_t trader, Side s, double price, std::uint32_t qty,
                        TimeInForce tif = TimeInForce::Day, bool post_only = false) {
        const std::size_t before = log.lines.size();
        eng.submit(NewOrder{trader, cid++, L, s, px(price), qty, tif, false, post_only}, ts++);
        return id_from(before);
    }
    std::uint64_t market(std::uint32_t trader, Side s, std::uint32_t qty) {
        const std::size_t before = log.lines.size();
        eng.submit(NewOrder{trader, cid++, L, s, 0, qty, TimeInForce::IOC, true, false}, ts++);
        return id_from(before);
    }
    void cancel(std::uint32_t trader, std::uint64_t id) { eng.cancel(CancelOrder{trader, id}, ts++); }
    void modify(std::uint32_t trader, std::uint64_t id, double price, std::uint32_t qty) {
        eng.modify(ModifyOrder{trader, id, px(price), qty}, ts++);
    }

    std::uint64_t id_from(std::size_t first_line) {
        if (first_line >= log.lines.size() || log.lines[first_line].rfind("ACK", 0) != 0) return 0;
        const auto& s = log.lines[first_line];
        return std::stoull(s.substr(s.find(" id=") + 4));
    }

    // Lines added since `from` that start with `prefix`.
    std::vector<std::string> since(std::size_t from, const std::string& prefix) {
        std::vector<std::string> out;
        for (std::size_t i = from; i < log.lines.size(); ++i)
            if (log.lines[i].rfind(prefix, 0) == 0) out.push_back(log.lines[i]);
        return out;
    }

    // (id, qty) for one side, priority order.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> queue(Side s) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> q;
        eng.for_each_resting(L, s, [&](std::uint64_t id, Price, std::uint32_t qty, std::uint32_t) { q.emplace_back(id, qty); });
        return q;
    }

    void TearDown() override {
        std::string why;
        EXPECT_TRUE(eng.check_invariants(&why)) << why;
    }
};

using Policies = ::testing::Types<book::VectorPolicy, book::MapPolicy>;
TYPED_TEST_SUITE(EngineTest, Policies);

bool has(const std::vector<std::string>& v, const std::string& needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

}  // namespace

// ---- 1. scenarios ------------------------------------------------------------

TYPED_TEST(EngineTest, PriceThenTimePriorityAndMakerPrice) {
    const auto a = this->limit(1, Side::Sell, 100.00, 100);
    const auto b = this->limit(2, Side::Sell, 100.00, 100);
    const auto c = this->limit(3, Side::Sell, 99.99, 100);
    const std::size_t mark = this->log.lines.size();
    this->limit(9, Side::Buy, 100.05, 250);  // sweeps C (best price), then A, then half of B

    const auto fills = this->since(mark, "FILL");
    ASSERT_EQ(fills.size(), 6u);  // 3 matches x (maker + taker)
    EXPECT_TRUE(has({fills[0]}, "id=" + std::to_string(c) + " "));
    EXPECT_TRUE(has({fills[0]}, "px=999900"));  // trades at the resting price, not 100.05
    EXPECT_TRUE(has({fills[2]}, "id=" + std::to_string(a) + " "));
    EXPECT_TRUE(has({fills[4]}, "id=" + std::to_string(b) + " "));
    EXPECT_TRUE(has({fills[4]}, "q=50 leaves=50"));
    EXPECT_EQ(this->queue(Side::Sell), (std::vector<std::pair<std::uint64_t, std::uint32_t>>{{b, 50}}));
    EXPECT_TRUE(this->queue(Side::Buy).empty());  // fully filled, nothing rests
}

TYPED_TEST(EngineTest, UnfilledRemainderRests) {
    this->limit(1, Side::Sell, 10.00, 100);
    const auto id = this->limit(2, Side::Buy, 10.00, 300);
    EXPECT_EQ(this->queue(Side::Buy), (std::vector<std::pair<std::uint64_t, std::uint32_t>>{{id, 200}}));
    EXPECT_EQ(this->eng.book(L)->last_price, px(10.00));
    EXPECT_EQ(this->eng.book(L)->volume, 100u);
}

TYPED_TEST(EngineTest, IocCancelsRemainder) {
    this->limit(1, Side::Sell, 10.00, 100);
    const std::size_t mark = this->log.lines.size();
    this->limit(2, Side::Buy, 10.00, 300, TimeInForce::IOC);
    EXPECT_TRUE(has(this->since(mark, "CXL"), "q=200 ioc remainder"));
    EXPECT_TRUE(this->queue(Side::Buy).empty());
}

TYPED_TEST(EngineTest, FokIsAllOrNothing) {
    this->limit(1, Side::Sell, 10.00, 100);
    this->limit(1, Side::Sell, 10.01, 100);
    std::size_t mark = this->log.lines.size();
    this->limit(2, Side::Buy, 10.01, 300, TimeInForce::FOK);  // only 200 available
    EXPECT_TRUE(this->since(mark, "FILL").empty());
    EXPECT_TRUE(has(this->since(mark, "CXL"), "fok unfilled"));
    EXPECT_EQ(this->queue(Side::Sell).size(), 2u);  // book untouched

    mark = this->log.lines.size();
    this->limit(2, Side::Buy, 10.01, 200, TimeInForce::FOK);  // exactly enough
    EXPECT_EQ(this->since(mark, "FILL").size(), 4u);
    EXPECT_TRUE(this->queue(Side::Sell).empty());
}

TYPED_TEST(EngineTest, PostOnlyNeverTakes) {
    this->limit(1, Side::Sell, 10.00, 100);
    const std::size_t mark = this->log.lines.size();
    EXPECT_EQ(this->limit(2, Side::Buy, 10.00, 100, TimeInForce::Day, true), 0u);
    EXPECT_TRUE(has(this->since(mark, "REJ"), "post-only would cross"));
    EXPECT_NE(this->limit(2, Side::Buy, 9.99, 100, TimeInForce::Day, true), 0u);  // adds liquidity: fine
}

TYPED_TEST(EngineTest, MarketOrderSweepsAndNeverRests) {
    this->limit(1, Side::Buy, 10.00, 100);
    this->limit(1, Side::Buy, 9.90, 100);
    const std::size_t mark = this->log.lines.size();
    this->market(2, Side::Sell, 500);
    const auto fills = this->since(mark, "FILL");
    ASSERT_EQ(fills.size(), 4u);
    EXPECT_TRUE(has({fills[2]}, "px=99000"));
    EXPECT_TRUE(has(this->since(mark, "CXL"), "q=300 ioc remainder"));
    EXPECT_TRUE(this->queue(Side::Sell).empty());
}

TYPED_TEST(EngineTest, ShrinkKeepsPriorityGrowLosesIt) {
    const auto a = this->limit(1, Side::Buy, 10.00, 300);
    const auto b = this->limit(2, Side::Buy, 10.00, 100);

    this->modify(1, a, 10.00, 200);  // smaller, same price: stays first, same id
    EXPECT_EQ(this->queue(Side::Buy), (std::vector<std::pair<std::uint64_t, std::uint32_t>>{{a, 200}, {b, 100}}));

    const std::size_t mark = this->log.lines.size();
    this->modify(1, a, 10.00, 400);  // bigger: new id, back of the line
    const auto rpl = this->since(mark, "RPL");
    ASSERT_EQ(rpl.size(), 1u);
    const std::uint64_t a2 = std::stoull(rpl[0].substr(rpl[0].find("new=") + 4));
    EXPECT_NE(a2, a);
    EXPECT_EQ(this->queue(Side::Buy), (std::vector<std::pair<std::uint64_t, std::uint32_t>>{{b, 100}, {a2, 400}}));
    EXPECT_EQ(this->eng.find(a), nullptr);
}

TYPED_TEST(EngineTest, ModifyThatCrossesTrades) {
    this->limit(1, Side::Sell, 10.05, 100);
    const auto bid = this->limit(2, Side::Buy, 10.00, 150);
    const std::size_t mark = this->log.lines.size();
    this->modify(2, bid, 10.05, 150);  // now marketable
    EXPECT_EQ(this->since(mark, "FILL").size(), 2u);
    ASSERT_EQ(this->queue(Side::Buy).size(), 1u);
    EXPECT_EQ(this->queue(Side::Buy)[0].second, 50u);  // remainder rests at 10.05
    EXPECT_EQ(this->eng.book(L)->best_bid(), px(10.05));
}

TYPED_TEST(EngineTest, SelfTradePreventionCancelsResting) {
    const auto mine = this->limit(7, Side::Sell, 10.00, 100);
    const auto other = this->limit(8, Side::Sell, 10.00, 100);
    const std::size_t mark = this->log.lines.size();
    this->limit(7, Side::Buy, 10.00, 100);  // would hit my own order first
    EXPECT_TRUE(has(this->since(mark, "CXL"), "id=" + std::to_string(mine) + " "));
    EXPECT_TRUE(has(this->since(mark, "CXL"), "self trade"));
    const auto fills = this->since(mark, "FILL");
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_TRUE(has({fills[0]}, "id=" + std::to_string(other) + " "));
    EXPECT_EQ(this->eng.stats().self_trade_cancels, 1u);
}

TYPED_TEST(EngineTest, RejectsBadRequests) {
    const std::size_t mark = this->log.lines.size();
    this->eng.submit(NewOrder{1, 1, 99, Side::Buy, px(10), 100}, this->ts++);   // unknown symbol
    this->limit(1, Side::Buy, 10.00, 0);                                         // zero qty
    this->limit(1, Side::Buy, 10.001, 100);                                      // off tick
    this->eng.submit(NewOrder{1, 1, L, Side::Buy, 0, 100, TimeInForce::IOC, true, true}, this->ts++);  // market + post-only
    this->cancel(1, 12345);                                                      // unknown order
    const auto id = this->limit(1, Side::Buy, 10.00, 100);
    this->cancel(2, id);                                                         // not the owner
    const auto rej = this->since(mark, "REJ");
    ASSERT_EQ(rej.size(), 6u);
    EXPECT_TRUE(has({rej[0]}, "unknown symbol"));
    EXPECT_TRUE(has({rej[1]}, "invalid qty"));
    EXPECT_TRUE(has({rej[2]}, "invalid price"));
    EXPECT_TRUE(has({rej[3]}, "invalid price"));
    EXPECT_TRUE(has({rej[4]}, "unknown order"));
    EXPECT_TRUE(has({rej[5]}, "not owner"));
    EXPECT_NE(this->eng.find(id), nullptr);  // the order survived
}

// ---- 2 & 3. differential fuzz + feed consistency ------------------------------

namespace {

struct FuzzConfig {
    std::uint64_t seed;
    int ops;
    int symbols;
    int traders;
};

template <typename Policy>
void fuzz(const FuzzConfig& fc) {
    EventLog real_log, ref_log;
    itch::Writer feed;
    MatchingEngine<EventLog, Policy> real(real_log, &feed);
    ReferenceEngine ref(ref_log);
    book::BookBuilder<book::VectorPolicy> rebuilt(1 << 12);
    itch::ParseStats pst;
    std::size_t parsed = 0;

    for (int s = 1; s <= fc.symbols; ++s) {
        real.add_symbol(std::uint16_t(s), "SYM" + std::to_string(s), 0);
        ref.add_symbol(std::uint16_t(s));
    }

    sim::Rng rng(fc.seed);
    std::map<std::uint64_t, std::pair<std::uint16_t, std::uint32_t>> live;  // id -> (locate, trader)
    std::uint64_t ts = 1, cid = 1;
    const Price mid = px(50.00);

    // Track which order ids are live from the reports themselves.
    auto track = [&](std::size_t from) {
        for (std::size_t i = from; i < real_log.lines.size(); ++i) {
            const std::string& l = real_log.lines[i];
            auto field = [&](const char* k) { return std::stoull(l.substr(l.find(k) + std::strlen(k))); };
            if (l.rfind("ACK", 0) == 0) live[field(" id=")] = {std::uint16_t(field(" loc=")), std::uint32_t(field(" tr="))};
            else if (l.rfind("CXL", 0) == 0) live.erase(field(" id="));
            else if (l.rfind("FILL", 0) == 0 && field(" leaves=") == 0) live.erase(field(" id="));
            else if (l.rfind("RPL", 0) == 0) {
                const auto old_id = field(" old="), new_id = field(" new=");
                if (old_id != new_id) {
                    live[new_id] = live[old_id];
                    live.erase(old_id);
                }
            }
        }
    };

    auto compare_books = [&](std::uint16_t loc, int op) {
        for (Side s : {Side::Buy, Side::Sell}) {
            std::vector<std::tuple<std::uint64_t, Price, std::uint32_t, std::uint32_t>> got;
            real.for_each_resting(loc, s, [&](std::uint64_t id, Price p, std::uint32_t q, std::uint32_t tr) {
                got.emplace_back(id, p, q, tr);
            });
            ASSERT_EQ(got, ref.side(loc, s)) << "book differs at op " << op;

            // Feed consistency: price levels rebuilt from ITCH == engine's levels.
            std::vector<std::tuple<Price, std::uint64_t, std::uint32_t>> from_feed, from_engine;
            const auto* fb = rebuilt.book(loc);
            auto lv = [&](const book::Level& l) { from_feed.emplace_back(l.price, l.qty, l.count); };
            if (fb) {
                if (s == Side::Buy) fb->bids.for_each(1000, lv);
                else fb->asks.for_each(1000, lv);
            }
            auto lv2 = [&](const book::Level& l) { from_engine.emplace_back(l.price, l.qty, l.count); };
            if (s == Side::Buy) real.book(loc)->bids.for_each(1000, lv2);
            else real.book(loc)->asks.for_each(1000, lv2);
            ASSERT_EQ(from_feed, from_engine) << "ITCH feed disagrees with engine at op " << op;
        }
    };

    for (int op = 0; op < fc.ops; ++op) {
        const std::size_t mark = real_log.lines.size();
        const auto r = rng.below(100);
        std::uint16_t loc = std::uint16_t(1 + rng.below(std::uint64_t(fc.symbols)));
        const std::uint32_t trader = std::uint32_t(1 + rng.below(std::uint64_t(fc.traders)));

        if (r < 55 || live.empty()) {
            NewOrder o{trader, cid++, loc, rng.chance(0.5) ? Side::Buy : Side::Sell, 0,
                       std::uint32_t(1 + rng.below(rng.chance(0.8) ? 300 : 3000))};
            // Prices cluster around mid so orders often cross.
            const std::int64_t off = std::int64_t(rng.below(21)) - 10;
            o.price = Price(std::int64_t(mid) + off * 100);
            const auto k = rng.below(100);
            if (k < 8) o.market = true;
            else if (k < 18) o.tif = TimeInForce::IOC;
            else if (k < 24) o.tif = TimeInForce::FOK;
            else if (k < 34) o.post_only = true;
            if (rng.chance(0.01)) o.qty = 0;                      // invalid
            if (rng.chance(0.01)) o.price += 1;                    // off tick
            if (rng.chance(0.005)) o.locate = 99;                  // unknown symbol
            real.submit(o, ts);
            ref.submit(o, ts);
        } else {
            // Pick a live order (sometimes a dead id, sometimes the wrong trader).
            auto it = live.begin();
            std::advance(it, long(rng.below(live.size())));
            std::uint64_t id = it->first;
            loc = it->second.first;
            std::uint32_t who = rng.chance(0.97) ? it->second.second : trader;
            if (rng.chance(0.02)) id += 1'000'000;
            if (r < 80) {
                real.cancel(CancelOrder{who, id}, ts);
                ref.cancel(CancelOrder{who, id}, ts);
            } else {
                const std::int64_t off = std::int64_t(rng.below(21)) - 10;
                ModifyOrder m{who, id, Price(std::int64_t(mid) + off * 100), std::uint32_t(1 + rng.below(500))};
                real.modify(m, ts);
                ref.modify(m, ts);
            }
        }
        ++ts;

        ASSERT_EQ(real_log.lines, ref_log.lines) << "reports differ at op " << op;
        track(mark);
        real_log.lines.clear();
        ref_log.lines.clear();

        parsed += itch::parse(feed.bytes().data() + parsed, feed.bytes().size() - parsed, rebuilt, pst);
        if (loc >= 1 && loc <= fc.symbols) compare_books(loc, op);
        if (op % 5000 == 0) {
            std::string why;
            ASSERT_TRUE(real.check_invariants(&why)) << "op " << op << ": " << why;
        }
    }
    for (int s = 1; s <= fc.symbols; ++s) compare_books(std::uint16_t(s), fc.ops);
    EXPECT_EQ(real.live_orders(), ref.live());
    EXPECT_EQ(rebuilt.stats().unknown_ref, 0u);
    EXPECT_EQ(rebuilt.stats().crossed_after_add, 0u);
    EXPECT_GT(real.stats().matches, 1000u);  // the fuzz really exercised matching
    std::string why;
    EXPECT_TRUE(real.check_invariants(&why)) << why;
}

}  // namespace

TEST(EngineFuzz, VectorBookSmallUniverse) { fuzz<book::VectorPolicy>({1, 60'000, 1, 3}); }
TEST(EngineFuzz, VectorBookManyTraders) { fuzz<book::VectorPolicy>({2, 60'000, 3, 12}); }
TEST(EngineFuzz, MapBook) { fuzz<book::MapPolicy>({3, 60'000, 2, 6}); }
