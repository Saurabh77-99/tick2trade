#pragma once
// Agent-based market simulation on top of the matching engine.
//
// Instead of generating fake ITCH messages (Phase 1's SyntheticFeed), this
// runs a small market: traders send real orders to the MatchingEngine, and
// the engine's ITCH output is the feed. Every message is therefore the
// consequence of an actual matching decision.
//
// It's a discrete-event simulation: each agent schedules its next wake-up
// time, and a priority queue always runs whichever agent wakes next. No
// threads and no real time, so the same seed always produces the same
// trading day, in seconds.
//
// The agents form a classic microstructure model:
//   * market makers quote both sides around their estimate of fair value,
//     earn the spread, and skew their quotes to shed inventory
//   * noise traders trade for reasons unrelated to price (random takers and
//     passive orders); on average they pay the spread
//   * informed traders know the true fair value and pick off stale quotes;
//     they're why market makers can't quote arbitrarily tight
//     ("adverse selection")
// Every trade's buyer and seller are both agents here, so total PnL across
// all agents must be exactly zero. The report checks that.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "hft/exchange/matching_engine.hpp"
#include "hft/itch/writer.hpp"
#include "hft/sim/rng.hpp"

namespace hft::sim {

using exchange::Price;
using exchange::Side;

struct MarketSimConfig {
    int symbols = 4;
    int market_makers = 3;
    int noise_traders = 30;
    int informed_traders = 2;
    double hours = 6.5;                  // 09:30 -> 16:00
    double vol_bps_per_sqrt_s = 2.0;     // fair value volatility (~3% per day)
    std::uint64_t seed = 7;
};

class MarketSim;

class Agent {
public:
    Agent(std::uint32_t id, std::string name, int symbols)
        : id_(id), name_(std::move(name)), position_(std::size_t(symbols) + 1, 0) {}
    virtual ~Agent() = default;

    virtual void act(MarketSim& sim, std::uint64_t now) = 0;
    virtual double mean_wake_s() const = 0;
    virtual const char* kind() const = 0;

    // ---- execution reports (delivered synchronously by the engine) ----
    virtual void on_accepted(const exchange::Accepted& e) {
        last_accepted_ = e.order_id;
        live_[e.order_id] = Live{e.locate, e.side, e.price, e.qty, e.ts};
        ++orders_;
    }
    virtual void on_rejected(const exchange::Rejected&) { ++rejects_; }
    virtual void on_fill(const exchange::Fill& f) {
        // Integer cash (Price units x shares) so the zero-sum check is exact.
        const std::int64_t notional = std::int64_t(f.price) * f.qty;
        if (f.side == Side::Buy) { position_[f.locate] += f.qty; cash_ -= notional; }
        else { position_[f.locate] -= f.qty; cash_ += notional; }
        volume_ += f.qty;
        ++fills_;
        if (f.leaves == 0) live_.erase(f.order_id);
        else if (auto it = live_.find(f.order_id); it != live_.end()) it->second.qty = f.leaves;
    }
    virtual void on_canceled(const exchange::Canceled& c) { live_.erase(c.order_id); }
    virtual void on_replaced(const exchange::Replaced& r) {
        auto it = live_.find(r.old_order_id);
        if (it == live_.end()) return;
        Live l = it->second;
        live_.erase(it);
        l.price = r.price;
        l.qty = r.qty;
        live_[r.new_order_id] = l;
    }

    // ---- results ----
    std::uint32_t id() const { return id_; }
    const std::string& name() const { return name_; }
    double cash() const { return double(cash_) / 10000.0; }  // dollars
    std::int64_t cash_raw() const { return cash_; }
    std::int64_t position(std::uint16_t loc) const { return position_[loc]; }
    std::uint64_t volume() const { return volume_; }
    std::uint64_t fills() const { return fills_; }
    std::uint64_t orders() const { return orders_; }

protected:
    struct Live {
        std::uint16_t locate;
        Side side;
        Price price;
        std::uint32_t qty;
        std::uint64_t since;
    };
    bool is_live(std::uint64_t id) const { return id && live_.count(id); }

    std::uint32_t id_;
    std::string name_;
    std::vector<std::int64_t> position_;
    std::int64_t cash_ = 0;  // Price units (1/10000 $)
    std::uint64_t volume_ = 0, fills_ = 0, orders_ = 0, rejects_ = 0;
    std::uint64_t last_accepted_ = 0;
    std::map<std::uint64_t, Live> live_;  // my resting orders
};

// Routes the engine's reports to the agent that owns the order.
struct Router : exchange::NullListener {
    std::vector<Agent*> agents;  // indexed by trader id
    void on_accepted(const exchange::Accepted& e) { agents[e.trader]->on_accepted(e); }
    void on_rejected(const exchange::Rejected& e) { agents[e.trader]->on_rejected(e); }
    void on_fill(const exchange::Fill& e) { agents[e.trader]->on_fill(e); }
    void on_canceled(const exchange::Canceled& e) { agents[e.trader]->on_canceled(e); }
    void on_replaced(const exchange::Replaced& e) { agents[e.trader]->on_replaced(e); }
};

class MarketSim {
public:
    static constexpr std::uint64_t kSecond = 1'000'000'000ull;
    static constexpr std::uint64_t kOpen = (9 * 3600 + 30 * 60) * kSecond;
    static constexpr Price kTick = 100;

    using Engine = exchange::MatchingEngine<Router>;

    MarketSim(MarketSimConfig cfg, itch::Writer& feed);

    // Runs the whole day; returns the number of agent actions.
    std::uint64_t run();
    void print_report(std::FILE* out) const;

    // ---- what agents can see and do ----
    Rng& rng() { return rng_; }
    double fair(std::uint16_t loc) const { return fair_[loc]; }
    Price best_bid(std::uint16_t loc) const { return engine_.book(loc)->best_bid(); }
    Price best_ask(std::uint16_t loc) const { return engine_.book(loc)->best_ask(); }
    std::uint32_t qty_at_best(std::uint16_t loc, Side side) const {
        const auto* b = engine_.book(loc);
        const book::Level* lv = side == Side::Buy ? b->bids.best() : b->asks.best();
        return lv ? std::uint32_t(lv->qty) : 0;
    }
    int symbols() const { return cfg_.symbols; }

    void submit(const exchange::NewOrder& o, std::uint64_t now) { engine_.submit(o, now); }
    void cancel(std::uint32_t trader, std::uint64_t id, std::uint64_t now) {
        engine_.cancel(exchange::CancelOrder{trader, id}, now);
    }
    void modify(std::uint32_t trader, std::uint64_t id, Price p, std::uint32_t q, std::uint64_t now) {
        engine_.modify(exchange::ModifyOrder{trader, id, p, q}, now);
    }
    std::uint64_t next_cid() { return ++cid_; }

    static Price to_price(double dollars) { return Price(std::llround(dollars * 100.0)) * kTick; }

    const Engine& engine() const { return engine_; }
    const std::vector<std::string>& symbol_names() const { return names_; }

private:
    void advance_fair(std::uint64_t now) {
        const double dt = double(now - fair_time_) / double(kSecond);
        if (dt <= 0) return;
        const double sigma = cfg_.vol_bps_per_sqrt_s * 1e-4 * std::sqrt(dt);
        for (int s = 1; s <= cfg_.symbols; ++s) fair_[s] *= std::exp(sigma * rng_.normal() - 0.5 * sigma * sigma);
        fair_time_ = now;
    }

    MarketSimConfig cfg_;
    itch::Writer& feed_;
    Rng rng_;
    Router router_;
    Engine engine_;
    std::vector<std::unique_ptr<Agent>> agents_;
    std::vector<double> fair_;
    std::vector<double> open_fair_;
    std::vector<std::string> names_;
    std::uint64_t fair_time_ = kOpen;
    std::uint64_t close_ = 0;
    std::uint64_t cid_ = 0;
};

// ---- agents -----------------------------------------------------------------

// Quotes a bid and an ask on every symbol around a noisy estimate of fair
// value. Inventory skew: when long, it shades both quotes down so it's more
// likely to sell than buy, and stops bidding entirely at its position limit.
class MarketMaker : public Agent {
public:
    MarketMaker(std::uint32_t id, std::string name, int symbols, int half_spread_ticks)
        : Agent(id, std::move(name), symbols), half_spread_(half_spread_ticks),
          bid_(std::size_t(symbols) + 1, 0), ask_(std::size_t(symbols) + 1, 0) {}

    const char* kind() const override { return "market maker"; }
    double mean_wake_s() const override { return 0.25; }

    void on_replaced(const exchange::Replaced& r) override {
        Agent::on_replaced(r);
        for (auto& v : bid_) if (v == r.old_order_id) v = r.new_order_id;
        for (auto& v : ask_) if (v == r.old_order_id) v = r.new_order_id;
    }

    void act(MarketSim& sim, std::uint64_t now) override {
        for (int s = 1; s <= sim.symbols(); ++s) {
            const auto loc = std::uint16_t(s);
            const double estimate = sim.fair(loc) * (1.0 + 1e-4 * 0.5 * sim.rng().normal());  // ~0.5 bp noise
            const double inv = double(position_[loc]);
            const double skew_ticks = -inv / 500.0;  // 1 tick per 500 shares
            const double mid_ticks = estimate * 100.0 + skew_ticks;
            Price bid = Price(std::floor(mid_ticks - half_spread_)) * MarketSim::kTick;
            Price ask = Price(std::ceil(mid_ticks + half_spread_)) * MarketSim::kTick;
            // Never cross the other side (that would take liquidity).
            if (const Price ba = sim.best_ask(loc); ba && bid >= ba) bid = ba - MarketSim::kTick;
            if (const Price bb = sim.best_bid(loc); bb && ask <= bb) ask = bb + MarketSim::kTick;

            const std::uint32_t size = 200 + 100 * std::uint32_t(sim.rng().below(4));
            quote(sim, now, loc, Side::Buy, bid_[loc], bid, size, inv < kMaxPosition);
            quote(sim, now, loc, Side::Sell, ask_[loc], ask, size, inv > -kMaxPosition);
        }
    }

private:
    static constexpr double kMaxPosition = 5000;

    void quote(MarketSim& sim, std::uint64_t now, std::uint16_t loc, Side side, std::uint64_t& id, Price price,
               std::uint32_t size, bool allowed) {
        if (!allowed) {
            if (is_live(id)) sim.cancel(id_, id, now);
            id = 0;
            return;
        }
        if (is_live(id)) {
            if (live_[id].price != price) sim.modify(id_, id, price, size, now);  // on_replaced updates id
            return;
        }
        last_accepted_ = 0;
        sim.submit(exchange::NewOrder{id_, sim.next_cid(), loc, side, price, size, exchange::TimeInForce::Day,
                                      false, /*post_only=*/true},
                   now);
        id = last_accepted_;
    }

    int half_spread_;
    std::vector<std::uint64_t> bid_, ask_;  // live quote id per symbol (0 = none)
};

// Trades for reasons unrelated to value: sometimes takes liquidity, sometimes
// leaves a passive order that it cancels after a while.
class NoiseTrader : public Agent {
public:
    using Agent::Agent;
    const char* kind() const override { return "noise trader"; }
    double mean_wake_s() const override { return 5.0; }

    void act(MarketSim& sim, std::uint64_t now) override {
        // Cancel stale passive orders (older than ~60 s).
        std::vector<std::uint64_t> stale;
        for (const auto& [oid, l] : live_)
            if (now - l.since > 60 * MarketSim::kSecond) stale.push_back(oid);
        for (auto oid : stale) sim.cancel(id_, oid, now);

        auto& rng = sim.rng();
        const auto loc = std::uint16_t(1 + rng.below(std::uint64_t(sim.symbols())));
        const Side side = rng.chance(0.5) ? Side::Buy : Side::Sell;
        const std::uint32_t qty = 100 * std::uint32_t(1 + rng.below(5));
        exchange::NewOrder o{id_, sim.next_cid(), loc, side, 0, qty};

        if (rng.chance(0.5)) {  // take liquidity
            if (rng.chance(0.3)) {
                o.market = true;
                o.tif = exchange::TimeInForce::IOC;
            } else {
                o.price = side == Side::Buy ? sim.best_ask(loc) : sim.best_bid(loc);
                o.tif = exchange::TimeInForce::IOC;
                if (!o.price) return;
            }
        } else {  // passive order a few ticks behind the best price
            const Price ref = side == Side::Buy ? sim.best_bid(loc) : sim.best_ask(loc);
            if (!ref) return;
            const Price back = Price(rng.geometric(0.4)) * MarketSim::kTick;
            o.price = side == Side::Buy ? ref - std::min(back, ref - MarketSim::kTick) : ref + back;
            o.post_only = true;
        }
        sim.submit(o, now);
    }
};

// Knows the true fair value. Buys when the best offer is clearly too cheap,
// sells when the best bid is clearly too rich.
class InformedTrader : public Agent {
public:
    using Agent::Agent;
    const char* kind() const override { return "informed trader"; }
    double mean_wake_s() const override { return 1.0; }

    void act(MarketSim& sim, std::uint64_t now) override {
        for (int s = 1; s <= sim.symbols(); ++s) {
            const auto loc = std::uint16_t(s);
            const double fair = sim.fair(loc) * 10000.0;  // in Price units
            const double edge = 1.5 * MarketSim::kTick;
            const double pos = double(position_[loc]);
            const Price ask = sim.best_ask(loc), bid = sim.best_bid(loc);
            if (ask && double(ask) < fair - edge && pos < kMaxPosition) {
                const std::uint32_t q = std::min<std::uint32_t>(300, sim.qty_at_best(loc, Side::Sell));
                sim.submit(exchange::NewOrder{id_, sim.next_cid(), loc, Side::Buy, ask, q, exchange::TimeInForce::IOC}, now);
            } else if (bid && double(bid) > fair + edge && pos > -kMaxPosition) {
                const std::uint32_t q = std::min<std::uint32_t>(300, sim.qty_at_best(loc, Side::Buy));
                sim.submit(exchange::NewOrder{id_, sim.next_cid(), loc, Side::Sell, bid, q, exchange::TimeInForce::IOC}, now);
            }
        }
    }

private:
    static constexpr double kMaxPosition = 3000;
};

// ---- MarketSim implementation -------------------------------------------------

inline MarketSim::MarketSim(MarketSimConfig cfg, itch::Writer& feed)
    : cfg_(cfg), feed_(feed), rng_(cfg.seed), engine_(router_, &feed, exchange::EngineConfig{}, 1 << 16) {
    static const char* kNames[] = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOGL", "META", "TSLA", "AVGO"};
    static const double kStart[] = {190.0, 420.0, 130.0, 185.0, 170.0, 500.0, 250.0, 160.0};
    cfg_.symbols = std::clamp(cfg_.symbols, 1, 8);
    close_ = kOpen + std::uint64_t(cfg_.hours * 3600.0 * double(kSecond));

    const std::uint64_t pre = 4 * 3600 * kSecond;
    feed_.system_event(3 * 3600 * kSecond, 'O');
    feed_.system_event(pre, 'S');
    fair_.assign(std::size_t(cfg_.symbols) + 1, 0.0);
    for (int s = 1; s <= cfg_.symbols; ++s) {
        names_.push_back(kNames[s - 1]);
        engine_.add_symbol(std::uint16_t(s), kNames[s - 1], pre + std::uint64_t(s));
        fair_[s] = kStart[s - 1];
    }
    open_fair_ = fair_;
    feed_.system_event(kOpen, 'Q');

    // Trader ids index Router::agents; id 0 is unused.
    router_.agents.push_back(nullptr);
    agents_.push_back(nullptr);
    std::uint32_t id = 1;
    for (int i = 0; i < cfg_.market_makers; ++i, ++id)
        agents_.push_back(std::make_unique<MarketMaker>(id, "mm-" + std::to_string(i + 1), cfg_.symbols, 1 + i % 2));
    for (int i = 0; i < cfg_.noise_traders; ++i, ++id)
        agents_.push_back(std::make_unique<NoiseTrader>(id, "noise-" + std::to_string(i + 1), cfg_.symbols));
    for (int i = 0; i < cfg_.informed_traders; ++i, ++id)
        agents_.push_back(std::make_unique<InformedTrader>(id, "informed-" + std::to_string(i + 1), cfg_.symbols));
    for (std::size_t i = 1; i < agents_.size(); ++i) router_.agents.push_back(agents_[i].get());
}

inline std::uint64_t MarketSim::run() {
    using Event = std::pair<std::uint64_t, std::uint32_t>;  // (wake time, agent id)
    std::priority_queue<Event, std::vector<Event>, std::greater<>> queue;  // earliest first
    auto schedule = [&](std::uint32_t id, std::uint64_t from) {
        const double s = rng_.exponential(agents_[id]->mean_wake_s());
        queue.emplace(from + 1 + std::uint64_t(s * double(kSecond)), id);
    };
    for (std::uint32_t id = 1; id < agents_.size(); ++id) schedule(id, kOpen);

    std::uint64_t actions = 0;
    while (!queue.empty()) {
        const auto [t, id] = queue.top();
        queue.pop();
        if (t >= close_) break;
        advance_fair(t);
        agents_[id]->act(*this, t);
        ++actions;
        schedule(id, t);
    }
    feed_.system_event(close_, 'M');
    feed_.system_event(20 * 3600 * kSecond, 'E');
    feed_.system_event(20 * 3600 * kSecond + 5 * 60 * kSecond, 'C');
    return actions;
}

inline void MarketSim::print_report(std::FILE* out) const {
    std::fprintf(out, "\n  %-7s %10s %10s %12s %12s %14s\n", "symbol", "open fair", "close fair", "last trade",
                 "volume", "bid / ask");
    for (int s = 1; s <= cfg_.symbols; ++s) {
        const auto* b = engine_.book(std::uint16_t(s));
        std::fprintf(out, "  %-7s %10.2f %10.2f %12.2f %12llu %6.2f / %.2f\n", names_[std::size_t(s - 1)].c_str(),
                     open_fair_[std::size_t(s)], fair_[std::size_t(s)], b->last_price / 10000.0,
                     (unsigned long long)b->volume, b->best_bid() / 10000.0, b->best_ask() / 10000.0);
    }

    struct Group { double pnl = 0; std::uint64_t volume = 0, fills = 0, orders = 0; int n = 0; };
    std::map<std::string, Group> groups;
    double total = 0;
    std::fprintf(out, "\n  %-12s %-16s %10s %12s %10s %14s\n", "agent", "type", "orders", "shares", "fills",
                 "PnL ($)");
    for (std::size_t i = 1; i < agents_.size(); ++i) {
        const Agent& a = *agents_[i];
        double pnl = a.cash();
        for (int s = 1; s <= cfg_.symbols; ++s) pnl += double(a.position(std::uint16_t(s))) * fair_[std::size_t(s)];
        total += pnl;
        Group& g = groups[a.kind()];
        g.pnl += pnl;
        g.volume += a.volume();
        g.fills += a.fills();
        g.orders += a.orders();
        ++g.n;
        if (std::string(a.kind()) != "noise trader")
            std::fprintf(out, "  %-12s %-16s %10llu %12llu %10llu %14.2f\n", a.name().c_str(), a.kind(),
                         (unsigned long long)a.orders(), (unsigned long long)a.volume(),
                         (unsigned long long)a.fills(), pnl);
    }
    std::fprintf(out, "  %-12s %-16s %10s %12s %10s %14s\n", "", "", "", "", "", "");
    for (const auto& [kind, g] : groups)
        std::fprintf(out, "  %-12s %-16s %10llu %12llu %10llu %14.2f\n", ("all x" + std::to_string(g.n)).c_str(),
                     kind.c_str(), (unsigned long long)g.orders, (unsigned long long)g.volume,
                     (unsigned long long)g.fills, g.pnl);
    // Every trade has a buyer and a seller among our agents, so cash and
    // shares must net to exactly zero across everyone (checked in integers).
    std::int64_t cash_sum = 0;
    bool shares_zero = true;
    for (std::size_t i = 1; i < agents_.size(); ++i) cash_sum += agents_[i]->cash_raw();
    for (int s = 1; s <= cfg_.symbols; ++s) {
        std::int64_t pos = 0;
        for (std::size_t i = 1; i < agents_.size(); ++i) pos += agents_[i]->position(std::uint16_t(s));
        shares_zero = shares_zero && pos == 0;
    }
    std::fprintf(out, "\n  sum of all PnL: %.2f   zero-sum check (exact): %s\n", total,
                 cash_sum == 0 && shares_zero ? "OK" : "FAILED - bug!");
}

}  // namespace hft::sim
