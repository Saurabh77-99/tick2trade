// itch snapshot: replay a file and write periodic book snapshots of one
// symbol as JSON lines, for the web viewer.
//
// Output (one JSON object per line):
//   {"type":"meta","symbol":"AAPL","interval_ms":1000,"depth":10,"from":...,"to":...}
//   {"type":"snap","t":<ns since midnight>,
//    "bids":[[price,shares,orders],...], "asks":[...],      best level first
//    "last":<price>, "vol":<cumulative shares>, "ntrades":<cumulative>,
//    "msgs":<messages for this symbol since previous snapshot>,
//    "trades":[[t,price,shares,"B"|"S"|"X"],...]}          since previous snapshot
//
// Prices stay as integers with 4 implied decimals (exactly as on the wire);
// the viewer divides by 10000 for display.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "args.hpp"
#include "hft/book/book_builder.hpp"
#include "hft/common/mapped_file.hpp"
#include "hft/common/timing.hpp"
#include "hft/itch/parser.hpp"

namespace hft::cli {

namespace {

using Builder = book::BookBuilder<book::VectorPolicy>;

struct TradePrint {
    std::uint64_t t;
    itch::Price price;
    std::uint32_t shares;
    char aggressor;  // 'B' buyer lifted the offer, 'S' seller hit the bid, 'X' cross
};

class SnapshotHandler : public itch::NullHandler {
public:
    SnapshotHandler(std::string symbol, std::uint64_t interval, std::uint64_t from, std::uint64_t to,
                    std::size_t depth, std::FILE* out)
        : symbol_(std::move(symbol)), interval_(interval), next_(from), to_(to), depth_(depth), out_(out) {}

    void on_system_event(const itch::SystemEvent& m) { tick(m.h); }
    void on_trading_action(const itch::StockTradingAction& m) { tick(m.h); }
    void on_other(char, const std::uint8_t*, std::size_t) {}

    void on_stock_directory(const itch::StockDirectory& m) {
        tick(m.h);
        if (itch::trim_stock(m.stock) == symbol_) target_ = m.h.locate;
        b_.on_stock_directory(m);
    }
    void on_add(const itch::AddOrder& m) { tick(m.h); b_.on_add(m); }
    void on_cancel(const itch::OrderCancel& m) { tick(m.h); b_.on_cancel(m); }
    void on_delete(const itch::OrderDelete& m) { tick(m.h); b_.on_delete(m); }
    void on_replace(const itch::OrderReplace& m) { tick(m.h); b_.on_replace(m); }

    void on_executed(const itch::OrderExecuted& m) {
        tick(m.h);
        if (const book::Order* o = b_.find_order(m.order_ref); o && o->locate == target_)
            print(m.h.timestamp, o->price, m.executed_shares, o->side == itch::Side::Sell ? 'B' : 'S');
        b_.on_executed(m);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        tick(m.h);
        if (const book::Order* o = b_.find_order(m.order_ref); o && o->locate == target_)
            print(m.h.timestamp, m.execution_price, m.executed_shares,
                  o->side == itch::Side::Sell ? 'B' : 'S');
        b_.on_executed_with_price(m);
    }
    void on_trade(const itch::Trade& m) {
        tick(m.h);
        if (m.h.locate == target_)
            print(m.h.timestamp, m.price, m.shares, m.side == itch::Side::Sell ? 'B' : 'S');
        b_.on_trade(m);
    }
    void on_cross_trade(const itch::CrossTrade& m) {
        tick(m.h);
        if (m.h.locate == target_ && m.shares)
            print(m.h.timestamp, m.cross_price, static_cast<std::uint32_t>(m.shares), 'X');
        b_.on_cross_trade(m);
    }

    // Emit any snapshots still due at end of file.
    void finish() {
        while (target_ >= 0 && next_ <= to_ && next_ <= last_ts_) emit();
    }

    int target() const { return target_; }
    std::size_t emitted() const { return emitted_; }

private:
    // Called before each message is applied: snapshots show the book as of
    // each interval boundary.
    void tick(const itch::Header& h) {
        last_ts_ = h.timestamp;
        while (target_ >= 0 && h.timestamp >= next_ && next_ <= to_) emit();
        if (h.locate == target_ && target_ > 0) ++msgs_;
    }

    void print(std::uint64_t t, itch::Price p, std::uint32_t shares, char side) {
        if (trades_.size() >= 200) trades_.erase(trades_.begin());  // keep the latest
        trades_.push_back({t, p, shares, side});
    }

    void emit() {
        const auto* b = b_.book(static_cast<std::uint16_t>(target_));
        std::string s;
        s.reserve(2048);
        s += "{\"type\":\"snap\",\"t\":" + std::to_string(next_) + ",\"bids\":[";
        auto levels = [&](const auto& side) {
            bool first = true;
            side.for_each(depth_, [&](const book::Level& lv) {
                if (!first) s += ',';
                first = false;
                s += '[' + std::to_string(lv.price) + ',' + std::to_string(lv.qty) + ',' +
                     std::to_string(lv.count) + ']';
            });
        };
        if (b) levels(b->bids);
        s += "],\"asks\":[";
        if (b) levels(b->asks);
        s += "],\"last\":" + std::to_string(b ? b->last_price : 0);
        s += ",\"vol\":" + std::to_string(b ? b->volume : 0);
        s += ",\"ntrades\":" + std::to_string(b ? b->trades : 0);
        s += ",\"msgs\":" + std::to_string(msgs_) + ",\"trades\":[";
        for (std::size_t i = 0; i < trades_.size(); ++i) {
            const auto& t = trades_[i];
            if (i) s += ',';
            s += '[' + std::to_string(t.t) + ',' + std::to_string(t.price) + ',' + std::to_string(t.shares) +
                 ",\"" + t.aggressor + "\"]";
        }
        s += "]}\n";
        std::fputs(s.c_str(), out_);
        trades_.clear();
        msgs_ = 0;
        next_ += interval_;
        ++emitted_;
    }

    Builder b_{1 << 16};
    std::string symbol_;
    std::uint64_t interval_, next_, to_;
    std::size_t depth_;
    std::FILE* out_;
    int target_ = -1;
    std::uint64_t last_ts_ = 0;
    std::uint64_t msgs_ = 0;
    std::size_t emitted_ = 0;
    std::vector<TradePrint> trades_;
};

}  // namespace

int cmd_snapshot(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("snapshot: missing file");
    if (!a.has("symbol")) throw std::runtime_error("snapshot: --symbol is required");

    const std::string symbol = a.str("symbol");
    const std::uint64_t interval = a.num("interval-ms", 1000) * 1'000'000ull;
    const std::uint64_t from = parse_clock(a.str("from", "09:30"));
    const std::uint64_t to = parse_clock(a.str("to", "16:00"));
    const std::size_t depth = a.num("depth", 10);
    if (interval == 0) throw std::runtime_error("--interval-ms must be > 0");

    const std::string out_path = a.str("out", symbol + ".jsonl");
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (!out) throw std::runtime_error("cannot write " + out_path);
    std::fprintf(out,
                 "{\"type\":\"meta\",\"symbol\":\"%s\",\"interval_ms\":%llu,\"depth\":%zu,\"from\":%llu,\"to\":%llu}\n",
                 symbol.c_str(), (unsigned long long)(interval / 1'000'000), depth, (unsigned long long)from,
                 (unsigned long long)to);

    MappedFile file(a.positional()[0]);
    SnapshotHandler h(symbol, interval, from, to, depth, out);
    itch::ParseStats st;
    const std::uint64_t t0 = now_ns();
    itch::parse(file.data(), file.size(), h, st);
    h.finish();
    std::fclose(out);

    if (h.target() < 0) throw std::runtime_error("symbol " + symbol + " not in file's stock directory");
    std::printf("wrote %zu snapshots of %s to %s (%.2f s)\n", h.emitted(), symbol.c_str(), out_path.c_str(),
                double(now_ns() - t0) / 1e9);
    return 0;
}

}  // namespace hft::cli
