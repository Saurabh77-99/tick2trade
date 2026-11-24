// itch sim: run an agent-based trading day through the matching engine and
// write the exchange's ITCH 5.0 market data feed to a file.
//
// The output works with every Phase 1 tool:
//   itch replay   sim.itch --check          (rebuilds the book from the feed)
//   itch snapshot sim.itch --symbol AAPL    (for the web viewer)

#include <cstdio>
#include <stdexcept>

#include "args.hpp"
#include "hft/common/timing.hpp"
#include "hft/sim/market_sim.hpp"

namespace hft::cli {

int cmd_sim(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("sim: missing output path");
    const std::string out = a.positional()[0];

    sim::MarketSimConfig cfg;
    cfg.symbols = static_cast<int>(a.num("symbols", 4));
    cfg.market_makers = static_cast<int>(a.num("mm", 3));
    cfg.noise_traders = static_cast<int>(a.num("noise", 30));
    cfg.informed_traders = static_cast<int>(a.num("informed", 2));
    cfg.seed = a.num("seed", 7);
    if (a.has("hours")) cfg.hours = std::stod(a.str("hours"));

    itch::Writer feed;
    feed.bytes().reserve(256u << 20);
    sim::MarketSim sim(cfg, feed);

    const std::uint64_t t0 = now_ns();
    const std::uint64_t actions = sim.run();
    const double secs = double(now_ns() - t0) / 1e9;

    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + out);
    std::fwrite(feed.bytes().data(), 1, feed.bytes().size(), f);
    std::fclose(f);

    const auto& st = sim.engine().stats();
    std::printf("simulated %.1f h: %d symbols, %d market makers, %d noise traders, %d informed traders (seed %llu)\n",
                cfg.hours, cfg.symbols, cfg.market_makers, cfg.noise_traders, cfg.informed_traders,
                (unsigned long long)cfg.seed);
    std::printf("agent actions %llu in %.2f s wall time\n", (unsigned long long)actions, secs);
    std::printf("engine: new orders %llu (accepted %llu, rejected %llu), modifies %llu, cancels %llu\n",
                (unsigned long long)st.new_orders, (unsigned long long)st.accepted, (unsigned long long)st.rejected,
                (unsigned long long)st.modifies, (unsigned long long)st.cancels);
    std::printf("        matches %llu, shares traded %llu, self-trade cancels %llu, resting at close %zu\n",
                (unsigned long long)st.matches, (unsigned long long)st.shares_traded,
                (unsigned long long)st.self_trade_cancels, sim.engine().live_orders());
    std::printf("wrote %s: %.1f MB of ITCH 5.0\n", out.c_str(), double(feed.bytes().size()) / 1e6);
    sim.print_report(stdout);
    return 0;
}

}  // namespace hft::cli
