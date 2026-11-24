// itch replay: rebuild every symbol's order book from an ITCH file.
//
// Reports throughput, data-quality counters (which double as a correctness
// test for the book), the most traded symbols, and optionally a per-message
// latency histogram and a ladder for one symbol.

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "args.hpp"
#include "hft/book/book_builder.hpp"
#include "hft/common/mapped_file.hpp"
#include "hft/common/timing.hpp"
#include "hft/itch/parser.hpp"

namespace hft::cli {

namespace {

// Like itch::parse(), but timestamps every message with the TSC. rdtsc adds
// ~10 ns per message, so use plain `replay` (no --latency) for throughput.
template <typename Handler>
void parse_timed(const std::uint8_t* data, std::size_t size, Handler& h, itch::ParseStats& st,
                 LatencyHistogram& hist) {
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len = load_be16(data + pos);
        if (pos + 2 + len > size) { st.truncated = true; break; }
        const std::uint8_t* msg = data + pos + 2;
        pos += 2 + std::size_t{len};
        if (len == 0) { ++st.bad_length; continue; }
        const char type = static_cast<char>(msg[0]);
        ++st.messages;
        ++st.by_type[static_cast<std::uint8_t>(type)];
        const std::uint16_t expected = itch::message_size(type);
        if (expected == 0) { ++st.unknown_type; continue; }
        if (len < expected) { ++st.bad_length; continue; }
        const std::uint64_t t0 = ticks();
        itch::dispatch(msg, len, h);
        hist.record(ticks() - t0);
    }
    st.bytes += pos;
}

// Read one byte per 4 KB page so the whole file is in the page cache before
// we start the clock. Otherwise the first run measures your disk.
std::uint64_t prefault(const MappedFile& f) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < f.size(); i += 4096) sum += f.data()[i];
    return sum;
}

std::string px(itch::Price p) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", double(p) / itch::kPriceScale);
    return b;
}

template <typename Book>
void print_ladder(const Book& b, const std::string& sym, std::size_t depth) {
    std::printf("\n%s order book (top %zu levels)\n", sym.c_str(), depth);
    std::printf("  %-4s %14s %12s %8s\n", "side", "price", "shares", "orders");
    std::vector<book::Level> asks;
    b.asks.for_each(depth, [&](const book::Level& lv) { asks.push_back(lv); });
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("  %-4s %14s %12llu %8u\n", "ASK", px(it->price).c_str(), (unsigned long long)it->qty, it->count);
    if (b.bids.best() && b.asks.best()) {
        const double spread = double(b.best_ask() - b.best_bid()) / itch::kPriceScale;
        const double mid = double(b.best_ask() + b.best_bid()) / 2.0 / itch::kPriceScale;
        std::printf("  ---- spread %.4f (%.1f bps) ----\n", spread, spread / mid * 1e4);
    } else {
        std::printf("  ---- one side empty ----\n");
    }
    b.bids.for_each(depth, [&](const book::Level& lv) {
        std::printf("  %-4s %14s %12llu %8u\n", "BID", px(lv.price).c_str(), (unsigned long long)lv.qty, lv.count);
    });
}

template <typename Policy>
int run(const Args& a, const MappedFile& file) {
    book::BookBuilder<Policy> builder(a.num("reserve", 1 << 16));
    itch::ParseStats st;
    LatencyHistogram hist;
    const bool latency = a.has("latency");
    const double tpn = latency ? calibrate_ticks_per_ns() : 1.0;

    const std::uint64_t t0 = now_ns();
    if (latency) parse_timed(file.data(), file.size(), builder, st, hist);
    else itch::parse(file.data(), file.size(), builder, st);
    const double secs = double(now_ns() - t0) / 1e9;

    const auto& bs = builder.stats();
    std::printf("book: %s   messages: %llu (%.1f MB)\n", Policy::name, (unsigned long long)st.messages,
                double(st.bytes) / 1e6);
    std::printf("parse + book: %.3f s, %.2f M msg/s, %.1f ns/msg%s\n", secs, double(st.messages) / secs / 1e6,
                secs * 1e9 / double(st.messages), latency ? "  (includes rdtsc overhead)" : "");
    std::printf("orders: adds %llu, executions %llu, cancels %llu, deletes %llu, replaces %llu\n",
                (unsigned long long)bs.adds, (unsigned long long)bs.executions, (unsigned long long)bs.cancels,
                (unsigned long long)bs.deletes, (unsigned long long)bs.replaces);
    std::printf("checks: unknown refs %llu, duplicate refs %llu, overfills %llu, crossed after add %llu\n",
                (unsigned long long)bs.unknown_ref, (unsigned long long)bs.duplicate_ref,
                (unsigned long long)bs.overfill, (unsigned long long)bs.crossed_after_add);
    std::printf("live orders at end: %zu, peak order pool size: %zu\n", builder.live_orders(),
                builder.pool().high_water());
    if (st.unknown_type || st.bad_length || st.truncated)
        std::printf("parse warnings: unknown types %llu, bad lengths %llu, truncated %s\n",
                    (unsigned long long)st.unknown_type, (unsigned long long)st.bad_length,
                    st.truncated ? "yes" : "no");

    // Most traded symbols.
    std::vector<std::uint16_t> locs;
    for (std::size_t i = 0; i < builder.num_locates(); ++i)
        if (builder.book(std::uint16_t(i))->volume) locs.push_back(std::uint16_t(i));
    std::sort(locs.begin(), locs.end(), [&](auto x, auto y) {
        return builder.book(x)->volume > builder.book(y)->volume;
    });
    if (!locs.empty()) {
        std::printf("\n  %-8s %14s %10s %12s %12s %12s\n", "symbol", "volume", "trades", "last", "bid", "ask");
        for (std::size_t i = 0; i < std::min<std::size_t>(10, locs.size()); ++i) {
            const auto* b = builder.book(locs[i]);
            std::printf("  %-8s %14llu %10llu %12s %12s %12s\n", builder.symbol(locs[i]).c_str(),
                        (unsigned long long)b->volume, (unsigned long long)b->trades, px(b->last_price).c_str(),
                        px(b->best_bid()).c_str(), px(b->best_ask()).c_str());
        }
    }

    if (a.has("symbol")) {
        const int loc = builder.locate_of(a.str("symbol"));
        if (loc < 0) std::printf("\nsymbol %s not found\n", a.str("symbol").c_str());
        else print_ladder(*builder.book(std::uint16_t(loc)), a.str("symbol"), a.num("depth", 10));
    }

    if (latency) {
        std::printf("\n");
        hist.print("per-message latency (parse + book update)", tpn);
    }

    if (a.has("check")) {
        std::string why;
        const std::uint64_t c0 = now_ns();
        const bool ok = builder.check_invariants(&why);
        std::printf("\ninvariant check: %s (%.2f s)%s%s\n", ok ? "PASS" : "FAIL", double(now_ns() - c0) / 1e9,
                    ok ? "" : " - ", why.c_str());
        if (!ok) return 1;
    }
    return 0;
}

}  // namespace

int cmd_replay(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("replay: missing file");
    MappedFile file(a.positional()[0]);
    if (a.has("warm")) {
        const std::uint64_t t0 = now_ns();
        const std::uint64_t s = prefault(file);
        std::printf("prefaulted %.1f MB in %.2f s (%llu)\n", double(file.size()) / 1e6,
                    double(now_ns() - t0) / 1e9, (unsigned long long)(s & 1));
    }
    const std::string kind = a.str("book", "vector");
    if (kind == "vector") return run<book::VectorPolicy>(a, file);
    if (kind == "map") return run<book::MapPolicy>(a, file);
    throw std::runtime_error("--book must be vector or map");
}

}  // namespace hft::cli
