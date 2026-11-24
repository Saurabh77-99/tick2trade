// bench_book: compare data structure choices on identical order flow.
//
//   ./bench_book                         synthetic day, 5M messages
//   ./bench_book --messages 20000000
//   ./bench_book --file 01302019.NASDAQ_ITCH50   a real ITCH file
//
// 1) Order map: FlatHashMap vs std::unordered_map, using an order-like
//    workload (sequential-ish refs, short-lived, a moving window of live keys).
// 2) Book: VectorPolicy vs MapPolicy, full replay, best of N runs, plus a
//    per-message latency histogram for each.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../src/tools/args.hpp"
#include "hft/book/book_builder.hpp"
#include "hft/common/flat_hash_map.hpp"
#include "hft/common/mapped_file.hpp"
#include "hft/common/timing.hpp"
#include "hft/itch/parser.hpp"
#include "hft/sim/rng.hpp"
#include "hft/sim/synthetic_feed.hpp"

using namespace hft;

namespace {

// Keep the optimizer from deleting work whose result we never use.
template <typename T>
inline void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

template <typename Map, typename Insert, typename Find, typename Erase>
double order_map_workload(Map& m, std::size_t ops, Insert ins, Find find, Erase erase) {
    // Mimic the exchange: refs increase; each new order lives for a random
    // number of further events; lookups hit recent orders most often.
    sim::Rng rng(7);
    std::vector<std::uint64_t> live;
    live.reserve(1 << 16);
    std::uint64_t ref = 1000, acc = 0;
    const std::uint64_t t0 = now_ns();
    for (std::size_t i = 0; i < ops; ++i) {
        const auto r = rng.below(100);
        const bool must_add = live.size() < 1000;      // keep the live set
        const bool must_erase = live.size() > 50000;   // between 1k and 50k
        if (must_add || (!must_erase && r < 40)) {     // add
            ref += 1 + rng.below(4);
            ins(m, ref, static_cast<std::uint32_t>(i));
            live.push_back(ref);
        } else if (!must_erase && r < 60) {            // lookup (exec / cancel)
            const std::size_t k = live.size() - 1 - std::min<std::size_t>(rng.geometric(0.01), live.size() - 1);
            acc += find(m, live[k]);
        } else {                                       // delete
            const std::size_t k = rng.below(live.size());
            acc += erase(m, live[k]);
            live[k] = live.back();
            live.pop_back();
        }
    }
    do_not_optimize(acc);
    return double(now_ns() - t0) / double(ops);
}

struct ReplayResult {
    double best_ns_per_msg = 1e18;
    std::uint64_t messages = 0;
    LatencyHistogram hist;
};

template <typename Policy>
ReplayResult bench_replay(const std::uint8_t* data, std::size_t size, int runs) {
    ReplayResult r;
    for (int i = 0; i < runs; ++i) {
        auto b = std::make_unique<book::BookBuilder<Policy>>(1 << 16);
        itch::ParseStats st;
        const std::uint64_t t0 = now_ns();
        itch::parse(data, size, *b, st);
        const double ns = double(now_ns() - t0) / double(st.messages);
        r.best_ns_per_msg = std::min(r.best_ns_per_msg, ns);
        r.messages = st.messages;
        if (b->stats().unknown_ref) std::printf("  WARNING: %llu unknown refs\n", (unsigned long long)b->stats().unknown_ref);
    }
    // One more pass with per-message timing.
    auto b = std::make_unique<book::BookBuilder<Policy>>(1 << 16);
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len = load_be16(data + pos);
        const std::uint8_t* msg = data + pos + 2;
        pos += 2 + std::size_t{len};
        if (itch::message_size(static_cast<char>(msg[0])) == 0) continue;
        const std::uint64_t t0 = ticks();
        itch::dispatch(msg, len, *b);
        r.hist.record(ticks() - t0);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const cli::Args args(argc, argv, 1);
    const int runs = static_cast<int>(args.num("runs", 3));
    const double tpn = calibrate_ticks_per_ns();
    std::printf("TSC: %.3f ticks/ns\n\n", tpn);

    // ---- 1. order map --------------------------------------------------------
    const std::size_t ops = args.num("map-ops", 20'000'000);
    std::printf("== order map: %zu ops (40%% insert, 20%% lookup, 40%% erase) ==\n", ops);
    {
        FlatHashMap fm(1 << 17);
        const double flat = order_map_workload(
            fm, ops, [](auto& m, auto k, auto v) { m.insert(k, v); },
            [](auto& m, auto k) { return m.find(k); }, [](auto& m, auto k) { return m.erase(k); });
        std::unordered_map<std::uint64_t, std::uint32_t> um;
        um.reserve(1 << 17);
        const double stdm = order_map_workload(
            um, ops, [](auto& m, auto k, auto v) { m.emplace(k, v); },
            [](auto& m, auto k) { auto it = m.find(k); return it == m.end() ? 0u : it->second; },
            [](auto& m, auto k) { return static_cast<std::uint32_t>(m.erase(k)); });
        std::printf("  FlatHashMap          %6.1f ns/op\n", flat);
        std::printf("  std::unordered_map   %6.1f ns/op   (%.2fx slower)\n\n", stdm, stdm / flat);
    }

    // ---- 2. book replay ------------------------------------------------------
    std::unique_ptr<MappedFile> file;
    itch::Writer w;
    const std::uint8_t* data;
    std::size_t size;
    if (args.has("file")) {
        file = std::make_unique<MappedFile>(args.str("file"));
        data = file->data();
        size = file->size();
        std::uint64_t s = 0;  // pull it into the page cache
        for (std::size_t i = 0; i < size; i += 4096) s += data[i];
        do_not_optimize(s);
        std::printf("== book replay: %s (%.1f MB), best of %d ==\n", args.str("file").c_str(), size / 1e6, runs);
    } else {
        sim::FeedConfig cfg;
        cfg.messages = args.num("messages", 5'000'000);
        cfg.symbols = static_cast<int>(args.num("symbols", 8));
        sim::SyntheticFeed(cfg).generate(w);
        data = w.bytes().data();
        size = w.bytes().size();
        std::printf("== book replay: synthetic, %llu messages, %d symbols, best of %d ==\n",
                    (unsigned long long)cfg.messages, cfg.symbols, runs);
    }

    const auto vec = bench_replay<book::VectorPolicy>(data, size, runs);
    const auto map = bench_replay<book::MapPolicy>(data, size, runs);
    std::printf("  vector levels   %6.1f ns/msg   %6.2f M msg/s\n", vec.best_ns_per_msg, 1e3 / vec.best_ns_per_msg);
    std::printf("  std::map levels %6.1f ns/msg   %6.2f M msg/s   (%.2fx slower)\n\n", map.best_ns_per_msg,
                1e3 / map.best_ns_per_msg, map.best_ns_per_msg / vec.best_ns_per_msg);
    vec.hist.print("vector levels: per-message latency", tpn);
    std::printf("\n");
    map.hist.print("std::map levels: per-message latency", tpn);
    return 0;
}
