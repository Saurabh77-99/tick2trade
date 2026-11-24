// Scenario tests for the order book. Each test runs against both level
// containers (TYPED_TEST), so the two implementations must agree.

#include <gtest/gtest.h>

#include <vector>

#include "hft/book/book_builder.hpp"
#include "hft/itch/writer.hpp"

using namespace hft;
using itch::Side;

namespace {

constexpr std::uint16_t L = 1;  // stock locate used throughout
constexpr itch::Price px(double dollars) { return static_cast<itch::Price>(dollars * 10000 + 0.5); }

template <typename Policy>
class BookTest : public ::testing::Test {
protected:
    book::BookBuilder<Policy> b{1024};
    itch::Writer w;
    std::uint64_t ts = 1;

    BookTest() {
        w.stock_directory(L, 0, "TEST");
        flush();
    }

    void flush() {
        itch::ParseStats st;
        itch::parse(w.bytes().data(), w.bytes().size(), b, st);
        w.clear();
        std::string why;
        ASSERT_TRUE(b.check_invariants(&why)) << why;
    }

    void add(std::uint64_t ref, Side s, std::uint32_t qty, itch::Price p) {
        w.add_order(L, ts++, ref, s, qty, "TEST", p);
        flush();
    }
    void exec(std::uint64_t ref, std::uint32_t qty) {
        w.order_executed(L, ts, ref, qty, /*match=*/ts);
        ++ts;
        flush();
    }
    void cancel(std::uint64_t ref, std::uint32_t qty) { w.order_cancel(L, ts++, ref, qty); flush(); }
    void del(std::uint64_t ref) { w.order_delete(L, ts++, ref); flush(); }
    void replace(std::uint64_t o, std::uint64_t n, std::uint32_t q, itch::Price p) {
        w.order_replace(L, ts++, o, n, q, p);
        flush();
    }

    const auto& book() { return *b.book(L); }

    // (price, qty, count) for the top n levels of one side.
    std::vector<std::tuple<itch::Price, std::uint64_t, std::uint32_t>> side(Side s, std::size_t n = 10) {
        std::vector<std::tuple<itch::Price, std::uint64_t, std::uint32_t>> out;
        auto f = [&](const book::Level& lv) { out.emplace_back(lv.price, lv.qty, lv.count); };
        if (s == Side::Buy) book().bids.for_each(n, f);
        else book().asks.for_each(n, f);
        return out;
    }
};

using Policies = ::testing::Types<book::VectorPolicy, book::MapPolicy>;
TYPED_TEST_SUITE(BookTest, Policies);

}  // namespace

TYPED_TEST(BookTest, LevelsAreSortedBestFirst) {
    this->add(1, Side::Buy, 100, px(10.00));
    this->add(2, Side::Buy, 200, px(10.02));
    this->add(3, Side::Buy, 300, px(9.99));
    this->add(4, Side::Sell, 100, px(10.05));
    this->add(5, Side::Sell, 100, px(10.03));
    this->add(6, Side::Sell, 100, px(10.10));

    using T = std::tuple<itch::Price, std::uint64_t, std::uint32_t>;
    EXPECT_EQ(this->side(Side::Buy),
              (std::vector<T>{{px(10.02), 200, 1}, {px(10.00), 100, 1}, {px(9.99), 300, 1}}));
    EXPECT_EQ(this->side(Side::Sell),
              (std::vector<T>{{px(10.03), 100, 1}, {px(10.05), 100, 1}, {px(10.10), 100, 1}}));
    EXPECT_EQ(this->book().best_bid(), px(10.02));
    EXPECT_EQ(this->book().best_ask(), px(10.03));
}

TYPED_TEST(BookTest, SamePriceAggregatesAndKeepsFifoOrder) {
    this->add(1, Side::Buy, 100, px(10.00));
    this->add(2, Side::Buy, 200, px(10.00));
    this->add(3, Side::Buy, 300, px(10.00));
    EXPECT_EQ(this->side(Side::Buy).size(), 1u);
    EXPECT_EQ(std::get<1>(this->side(Side::Buy)[0]), 600u);
    EXPECT_EQ(std::get<2>(this->side(Side::Buy)[0]), 3u);
    EXPECT_EQ(this->b.queue_position(1), 0u);
    EXPECT_EQ(this->b.queue_position(2), 1u);
    EXPECT_EQ(this->b.queue_position(3), 2u);

    this->exec(1, 100);  // the oldest order fills first and leaves
    EXPECT_EQ(this->b.find_order(1), nullptr);
    EXPECT_EQ(this->b.queue_position(2), 0u);
    EXPECT_EQ(this->b.queue_position(3), 1u);
}

TYPED_TEST(BookTest, PartialExecutionAndCancelReduceShares) {
    this->add(1, Side::Sell, 500, px(20.00));
    this->exec(1, 200);
    EXPECT_EQ(this->b.find_order(1)->shares, 300u);
    this->cancel(1, 100);
    EXPECT_EQ(this->b.find_order(1)->shares, 200u);
    EXPECT_EQ(std::get<1>(this->side(Side::Sell)[0]), 200u);
    EXPECT_EQ(this->book().volume, 200u);  // cancels don't add volume
    EXPECT_EQ(this->book().last_price, px(20.00));
}

TYPED_TEST(BookTest, EmptyLevelsDisappear) {
    this->add(1, Side::Buy, 100, px(10.00));
    this->add(2, Side::Buy, 100, px(9.99));
    this->del(1);
    EXPECT_EQ(this->book().best_bid(), px(9.99));
    this->exec(2, 100);
    EXPECT_TRUE(this->side(Side::Buy).empty());
    EXPECT_EQ(this->book().best_bid(), 0u);
    EXPECT_EQ(this->b.live_orders(), 0u);
}

TYPED_TEST(BookTest, ReplaceLosesTimePriority) {
    this->add(1, Side::Buy, 100, px(10.00));
    this->add(2, Side::Buy, 100, px(10.00));
    this->replace(1, 10, 150, px(10.00));  // same price, new size
    EXPECT_EQ(this->b.find_order(1), nullptr);
    ASSERT_NE(this->b.find_order(10), nullptr);
    EXPECT_EQ(this->b.queue_position(2), 0u);   // order 2 is now first in line
    EXPECT_EQ(this->b.queue_position(10), 1u);
    EXPECT_EQ(std::get<1>(this->side(Side::Buy)[0]), 250u);

    this->replace(10, 11, 100, px(10.01));  // price change moves it to a new level
    EXPECT_EQ(this->book().best_bid(), px(10.01));
    EXPECT_EQ(this->side(Side::Buy).size(), 2u);
    EXPECT_EQ(this->b.find_order(11)->side, Side::Buy);  // side is inherited
}

TYPED_TEST(BookTest, BadInputIsCountedNotFatal) {
    this->exec(999, 100);
    this->cancel(999, 100);
    this->del(999);
    this->replace(999, 1000, 100, px(1.00));
    EXPECT_EQ(this->b.stats().unknown_ref, 4u);

    this->add(1, Side::Buy, 100, px(10.00));
    this->exec(1, 150);  // more than resting: order removed, overfill counted
    EXPECT_EQ(this->b.stats().overfill, 1u);
    EXPECT_EQ(this->b.find_order(1), nullptr);

    this->add(2, Side::Buy, 100, px(10.00));
    this->add(2, Side::Buy, 300, px(10.01));  // duplicate ref replaces the old order
    EXPECT_EQ(this->b.stats().duplicate_ref, 1u);
    EXPECT_EQ(this->b.live_orders(), 1u);
    EXPECT_EQ(this->book().best_bid(), px(10.01));
}

TYPED_TEST(BookTest, DetectsCrossedBook) {
    this->add(1, Side::Sell, 100, px(10.00));
    this->add(2, Side::Buy, 100, px(9.99));
    EXPECT_EQ(this->b.stats().crossed_after_add, 0u);
    this->add(3, Side::Buy, 100, px(10.00));  // locks the book: bid == ask
    EXPECT_EQ(this->b.stats().crossed_after_add, 1u);
    EXPECT_TRUE(this->book().crossed());
}

TYPED_TEST(BookTest, PoolSlotsAreReused) {
    for (std::uint64_t i = 1; i <= 100; ++i) {
        this->add(i, Side::Buy, 100, px(10.00));
        this->del(i);
    }
    EXPECT_EQ(this->b.pool().high_water(), 1u);  // one slot, recycled 100 times
}
