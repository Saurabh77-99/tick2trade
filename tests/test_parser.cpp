#include <gtest/gtest.h>

#include <vector>

#include "hft/itch/parser.hpp"
#include "hft/itch/writer.hpp"

using namespace hft;
using namespace hft::itch;

namespace {

// Records every decoded message so tests can inspect the fields.
struct Recorder : NullHandler {
    std::vector<SystemEvent> sys;
    std::vector<StockDirectory> dir;
    std::vector<StockTradingAction> act;
    std::vector<AddOrder> adds;
    std::vector<OrderExecuted> execs;
    std::vector<OrderExecutedWithPrice> execs_px;
    std::vector<OrderCancel> cancels;
    std::vector<OrderDelete> deletes;
    std::vector<OrderReplace> replaces;
    std::vector<Trade> trades;
    std::vector<CrossTrade> crosses;
    std::vector<BrokenTrade> broken;
    std::vector<char> other;

    void on_system_event(const SystemEvent& m) { sys.push_back(m); }
    void on_stock_directory(const StockDirectory& m) { dir.push_back(m); }
    void on_trading_action(const StockTradingAction& m) { act.push_back(m); }
    void on_add(const AddOrder& m) { adds.push_back(m); }
    void on_executed(const OrderExecuted& m) { execs.push_back(m); }
    void on_executed_with_price(const OrderExecutedWithPrice& m) { execs_px.push_back(m); }
    void on_cancel(const OrderCancel& m) { cancels.push_back(m); }
    void on_delete(const OrderDelete& m) { deletes.push_back(m); }
    void on_replace(const OrderReplace& m) { replaces.push_back(m); }
    void on_trade(const Trade& m) { trades.push_back(m); }
    void on_cross_trade(const CrossTrade& m) { crosses.push_back(m); }
    void on_broken_trade(const BrokenTrade& m) { broken.push_back(m); }
    void on_other(char t, const std::uint8_t*, std::size_t) { other.push_back(t); }
};

}  // namespace

TEST(Parser, RoundTripsEveryMessageType) {
    Writer w;
    const std::uint64_t ts = 34'200'000'000'123ull;  // 09:30:00.000000123
    w.system_event(ts, 'Q');
    w.stock_directory(7, ts, "AAPL", 100);
    w.trading_action(7, ts, "AAPL", 'T');
    w.add_order(7, ts, 1001, Side::Buy, 300, "AAPL", 1874500);
    w.add_order(7, ts + 1, 1002, Side::Sell, 200, "AAPL", 1874600, "GSCO");
    w.order_executed(7, ts + 2, 1001, 100, 555);
    w.order_executed_with_price(7, ts + 3, 1001, 50, 556, 1874400, 'N');
    w.order_cancel(7, ts + 4, 1002, 100);
    w.order_replace(7, ts + 5, 1002, 1003, 400, 1874700);
    w.order_delete(7, ts + 6, 1003);
    w.trade(7, ts + 7, Side::Buy, 25, "AAPL", 1874550, 557);
    w.cross_trade(7, ts + 8, 123456, "AAPL", 1874500, 558, 'O');
    w.broken_trade(7, ts + 9, 557);

    Recorder r;
    ParseStats st;
    const auto consumed = parse(w.bytes().data(), w.bytes().size(), r, st);
    EXPECT_EQ(consumed, w.bytes().size());
    EXPECT_EQ(st.messages, 13u);
    EXPECT_EQ(st.bad_length, 0u);
    EXPECT_FALSE(st.truncated);

    ASSERT_EQ(r.sys.size(), 1u);
    EXPECT_EQ(r.sys[0].event_code, 'Q');
    EXPECT_EQ(r.sys[0].h.timestamp, ts);

    ASSERT_EQ(r.dir.size(), 1u);
    EXPECT_EQ(trim_stock(r.dir[0].stock), "AAPL");
    EXPECT_EQ(r.dir[0].h.locate, 7);
    EXPECT_EQ(r.dir[0].round_lot_size, 100u);

    ASSERT_EQ(r.act.size(), 1u);
    EXPECT_EQ(r.act[0].trading_state, 'T');

    ASSERT_EQ(r.adds.size(), 2u);
    EXPECT_EQ(r.adds[0].h.type, 'A');
    EXPECT_EQ(r.adds[0].order_ref, 1001u);
    EXPECT_EQ(r.adds[0].side, Side::Buy);
    EXPECT_EQ(r.adds[0].shares, 300u);
    EXPECT_EQ(r.adds[0].price, 1874500u);
    EXPECT_EQ(trim_stock(r.adds[0].stock), "AAPL");
    EXPECT_EQ(r.adds[1].h.type, 'F');
    EXPECT_EQ(r.adds[1].side, Side::Sell);
    EXPECT_EQ(std::string(r.adds[1].mpid, 4), "GSCO");
    EXPECT_EQ(r.adds[1].h.timestamp, ts + 1);

    ASSERT_EQ(r.execs.size(), 1u);
    EXPECT_EQ(r.execs[0].order_ref, 1001u);
    EXPECT_EQ(r.execs[0].executed_shares, 100u);
    EXPECT_EQ(r.execs[0].match_number, 555u);

    ASSERT_EQ(r.execs_px.size(), 1u);
    EXPECT_EQ(r.execs_px[0].execution_price, 1874400u);
    EXPECT_EQ(r.execs_px[0].printable, 'N');

    ASSERT_EQ(r.cancels.size(), 1u);
    EXPECT_EQ(r.cancels[0].cancelled_shares, 100u);

    ASSERT_EQ(r.replaces.size(), 1u);
    EXPECT_EQ(r.replaces[0].original_ref, 1002u);
    EXPECT_EQ(r.replaces[0].new_ref, 1003u);
    EXPECT_EQ(r.replaces[0].shares, 400u);
    EXPECT_EQ(r.replaces[0].price, 1874700u);

    ASSERT_EQ(r.deletes.size(), 1u);
    EXPECT_EQ(r.deletes[0].order_ref, 1003u);

    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].shares, 25u);
    EXPECT_EQ(r.trades[0].match_number, 557u);

    ASSERT_EQ(r.crosses.size(), 1u);
    EXPECT_EQ(r.crosses[0].shares, 123456u);
    EXPECT_EQ(r.crosses[0].cross_type, 'O');

    ASSERT_EQ(r.broken.size(), 1u);
    EXPECT_EQ(r.broken[0].match_number, 557u);
}

TEST(Parser, StopsCleanlyOnTruncatedInput) {
    Writer w;
    w.add_order(1, 1, 1, Side::Buy, 100, "X", 10000);
    w.add_order(1, 2, 2, Side::Buy, 100, "X", 10000);
    Recorder r;
    ParseStats st;
    const auto consumed = parse(w.bytes().data(), w.bytes().size() - 5, r, st);
    EXPECT_EQ(consumed, 2u + 36u);  // only the first message
    EXPECT_EQ(r.adds.size(), 1u);
    EXPECT_TRUE(st.truncated);
}

TEST(Parser, RejectsShortMessagesAndCountsUnknownTypes) {
    std::vector<std::uint8_t> buf = {
        0x00, 0x05, 'A', 0, 0, 0, 0,  // 'A' claiming 5 bytes: too short
        0x00, 0x03, 'z', 1, 2,        // unknown type
    };
    Recorder r;
    ParseStats st;
    parse(buf.data(), buf.size(), r, st);
    EXPECT_EQ(r.adds.size(), 0u);
    EXPECT_EQ(st.bad_length, 1u);
    EXPECT_EQ(st.unknown_type, 1u);
    EXPECT_EQ(st.messages, 2u);
}

TEST(Parser, MessageSizesMatchSpec) {
    EXPECT_EQ(message_size('A'), 36);
    EXPECT_EQ(message_size('F'), 40);
    EXPECT_EQ(message_size('E'), 31);
    EXPECT_EQ(message_size('C'), 36);
    EXPECT_EQ(message_size('X'), 23);
    EXPECT_EQ(message_size('D'), 19);
    EXPECT_EQ(message_size('U'), 35);
    EXPECT_EQ(message_size('P'), 44);
    EXPECT_EQ(message_size('R'), 39);
    EXPECT_EQ(message_size('?'), 0);
}
