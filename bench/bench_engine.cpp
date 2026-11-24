// bench_engine: how fast is the matching engine?
//
//   ./bench_engine                     2M requests, 4 symbols
//   ./bench_engine --requests 10000000
//
// Method:
//  1. Generate a realistic request stream once: mostly passive orders near
//     the top of book, cancels, modifies, and some aggressive orders that
//     trade. Generation needs to know which orders are live, so it runs
//     against an engine.
//  2. Because the engine is deterministic, replaying the recorded requests
//     into a fresh engine reproduces exactly the same behaviour. So the
//     timed runs measure only the engine, not the generator.
//  3. Report throughput (best of N untimed runs) and a per-request latency
//     histogram, for each book layout, with and without ITCH output.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

#include "../src/tools/args.hpp"
#include "hft/common/timing.hpp"
#include "hft/exchange/matching_engine.hpp"
#include "hft/sim/rng.hpp"

using namespace hft;
using namespace hft::exchange;

namespace {

struct Request {
    enum Kind : std::uint8_t { New, Cancel, Modify } kind;
    NewOrder n;
    CancelOrder c;
    ModifyOrder m;
};

struct Mix {
    std::uint64_t passive = 0, aggressive = 0, cancels = 0, modifies = 0;
};

std::vector<Request> generate(std::size_t count, int symbols, std::uint64_t seed, Mix& mix) {
    NullListener null;
    MatchingEngine<NullListener> eng(null);
    for (int s = 1; s <= symbols; ++s) eng.add_symbol(std::uint16_t(s), "S" + std::to_string(s), 0);

    sim::Rng rng(seed);
    std::vector<double> mid(std::size_t(symbols) + 1, 0);
    for (int s = 1; s <= symbols; ++s) mid[std::size_t(s)] = 5000 + 100.0 * s;  // in ticks ($50+)
    std::vector<std::pair<std::uint64_t, std::uint32_t>> live;  // (order id, trader); lazily pruned
    std::uint64_t next_id = 1, cid = 0;  // engine ids are sequential from 1
    std::vector<Request> out;
    out.reserve(count);

    auto pick_live = [&](std::uint64_t& id, std::uint32_t& trader) {
        while (!live.empty()) {
            const std::size_t k = rng.below(live.size());
            if (eng.find(live[k].first)) {
                id = live[k].first;
                trader = live[k].second;
                return true;
            }
            live[k] = live.back();
            live.pop_back();
        }
        return false;
    };

    for (std::size_t i = 0; i < count; ++i) {
        const auto loc = std::uint16_t(1 + rng.below(std::uint64_t(symbols)));
        double& m = mid[loc];
        if (rng.chance(0.01)) m += rng.chance(0.5) ? 1 : -1;
        const std::uint32_t trader = std::uint32_t(1 + rng.below(20));
        const auto r = rng.below(100);

        Request req{};
        std::uint64_t id = 0;
        std::uint32_t owner = 0;
        const bool too_many = live.size() > 20000;
        if (!too_many && (r < 55 || live.empty())) {
            const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
            NewOrder o{trader, ++cid, loc, side, 0, 100 * std::uint32_t(1 + rng.below(5))};
            if (r < 45) {  // passive, near the top
                const double off = 1 + rng.geometric(0.35);
                o.price = Price((side == Side::Buy ? m - off : m + off)) * 100;
                ++mix.passive;
            } else {       // aggressive: through the touch, IOC
                o.price = Price((side == Side::Buy ? m + 2 : m - 2)) * 100;
                o.tif = TimeInForce::IOC;
                ++mix.aggressive;
            }
            req.kind = Request::New;
            req.n = o;
            eng.submit(o, i);
            live.emplace_back(next_id++, trader);  // every valid order gets the next id
        } else if (pick_live(id, owner)) {
            if (r < 85 || too_many) {
                req.kind = Request::Cancel;
                req.c = CancelOrder{owner, id};
                eng.cancel(req.c, i);
                ++mix.cancels;
            } else {
                const book::Order* o = eng.find(id);
                const double off = 1 + rng.geometric(0.35);
                const Price p = Price((o->side == Side::Buy ? m - off : m + off)) * 100;
                req.kind = Request::Modify;
                req.m = ModifyOrder{owner, id, p, o->shares};
                const bool new_id = p != o->price;
                eng.modify(req.m, i);
                if (new_id) live.emplace_back(next_id++, owner);
                ++mix.modifies;
            }
        } else {
            --i;
            continue;
        }
        out.push_back(req);
    }
    return out;
}

template <typename Engine>
inline void apply(Engine& eng, const Request& r, std::uint64_t ts) {
    switch (r.kind) {
        case Request::New: eng.submit(r.n, ts); break;
        case Request::Cancel: eng.cancel(r.c, ts); break;
        case Request::Modify: eng.modify(r.m, ts); break;
    }
}

struct Result {
    double best_ns = 1e18;
    LatencyHistogram hist;
    EngineStats stats;
};

template <typename Policy>
Result run(const std::vector<Request>& reqs, int symbols, bool feed, int runs) {
    Result res;
    NullListener null;
    for (int pass = 0; pass <= runs; ++pass) {
        const bool timed_each = pass == runs;  // last pass: per-request histogram
        itch::Writer w;
        w.bytes().reserve(reqs.size() * 40);  // no reallocation inside the timed loop
        auto eng = std::make_unique<MatchingEngine<NullListener, Policy>>(null, feed ? &w : nullptr,
                                                                          EngineConfig{}, 1 << 16);
        for (int s = 1; s <= symbols; ++s) eng->add_symbol(std::uint16_t(s), "S" + std::to_string(s), 0);

        if (timed_each) {
            for (std::size_t i = 0; i < reqs.size(); ++i) {
                const std::uint64_t t0 = ticks();
                apply(*eng, reqs[i], i);
                res.hist.record(ticks() - t0);
            }
            res.stats = eng->stats();
        } else {
            const std::uint64_t t0 = now_ns();
            for (std::size_t i = 0; i < reqs.size(); ++i) apply(*eng, reqs[i], i);
            res.best_ns = std::min(res.best_ns, double(now_ns() - t0) / double(reqs.size()));
        }
    }
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    const cli::Args args(argc, argv, 1);
    const std::size_t n = args.num("requests", 2'000'000);
    const int symbols = int(args.num("symbols", 4));
    const int runs = int(args.num("runs", 3));
    const double tpn = calibrate_ticks_per_ns();

    Mix mix;
    const auto reqs = generate(n, symbols, args.num("seed", 1), mix);
    auto pct = [&](std::uint64_t v) { return 100.0 * double(v) / double(reqs.size()); };
    std::printf("== matching engine: %zu requests, %d symbols ==\n", reqs.size(), symbols);
    std::printf("   mix: %.0f%% passive new, %.0f%% aggressive IOC, %.0f%% cancel, %.0f%% modify\n\n",
                pct(mix.passive), pct(mix.aggressive), pct(mix.cancels), pct(mix.modifies));

    struct Row { const char* name; Result r; };
    std::vector<Row> rows;
    rows.push_back({"vector book", run<book::VectorPolicy>(reqs, symbols, false, runs)});
    rows.push_back({"vector book + ITCH feed", run<book::VectorPolicy>(reqs, symbols, true, runs)});
    rows.push_back({"std::map book", run<book::MapPolicy>(reqs, symbols, false, runs)});

    const auto& st = rows[0].r.stats;
    std::printf("   outcome: %llu matches, %llu shares traded, %llu rejects\n\n", (unsigned long long)st.matches,
                (unsigned long long)st.shares_traded, (unsigned long long)st.rejected);

    std::printf("  %-26s %9s %9s %9s %9s %9s %11s\n", "configuration", "ns/req", "M req/s", "p50", "p99", "p99.9",
                "max");
    for (const auto& row : rows) {
        const auto& h = row.r.hist;
        std::printf("  %-26s %9.1f %9.2f %9.0f %9.0f %9.0f %11.0f\n", row.name, row.r.best_ns, 1e3 / row.r.best_ns,
                    double(h.percentile(0.5)) / tpn, double(h.percentile(0.99)) / tpn,
                    double(h.percentile(0.999)) / tpn, double(h.max()) / tpn);
    }
    std::printf("\n  ns/req: best of %d untimed runs. Percentiles: per-request rdtsc (adds ~10 ns), in ns.\n", runs);
    return 0;
}
