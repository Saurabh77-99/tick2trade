# Phase 2 design notes: the exchange

Why the matching engine and the simulation are built the way they are. Each section ends with questions an interviewer could ask; answer them from your own experiments.

---

## 1. Matching rules: price-time priority

A buy order at price P trades with resting sells priced at or below P: the best (lowest) price first, and within a price, the oldest order first. Every trade executes at the **resting** order's price. Whatever doesn't trade either rests in the book (DAY), is canceled (IOC, market), or the whole order is rejected if it can't fill completely (FOK).

Price-time priority (FIFO) is what Nasdaq, NYSE and most equity exchanges use. It rewards being first, which is exactly why HFT firms spend so much on latency: queue position is worth money.

*Questions:* Why does the trade happen at the resting price and not the aggressor's limit? What is pro-rata matching, where is it used (some futures markets), and how does it change trader behaviour? Why do people "join the queue" early at a price they don't yet want to trade at?

## 2. Modify semantics and queue priority

- Shrinking an order at the same price **keeps** its queue position. It can't hurt anyone behind it.
- Growing it, or changing its price, **loses** the position: new id, back of the line. Otherwise you could hold a tiny order at the front and grow it when convenient.
- A modify that crosses the spread is treated as a new aggressive order and trades immediately.

The ITCH feed reflects each case: `X` (partial cancel) for a shrink, `U` (replace, new reference number) for a priority loss, and `D` followed by executions and possibly an `A` for a crossing modify.

*Questions:* Why can't an exchange let a size increase keep its priority? Which ITCH message does each kind of modify produce, and why does the crossing case need `D` + `A` instead of `U`?

## 3. Self-trade prevention

A trader's incoming order never trades against its own resting order; the resting one is canceled. Trading with yourself is "wash trading": it fakes volume and is prohibited in most markets. Exchanges offer STP modes (cancel resting, cancel incoming, cancel both, decrement); this engine implements cancel-resting.

FOK interacts with STP: the fill-or-kill check must ignore your own orders, because STP would cancel them, not fill against them.

*Questions:* Why do exchanges enforce STP instead of leaving it to firms? How should FOK and STP interact, and what would go wrong if the FOK check counted your own orders?

## 4. Hot/cold data split

While matching, the engine touches each order's price, size and FIFO links. Those live in the 32-byte `Order` from Phase 1, two per cache line. Who owns the order and their client id are needed only when sending reports, so they sit in a separate `meta_` array indexed the same way. Cold fields don't take up cache space in the matching loop.

This is the struct-of-arrays idea used everywhere in performance work, from game engines to databases.

*Questions:* What's the difference between array-of-structs and struct-of-arrays? When does splitting hot and cold data help, and when does the extra indirection cost more than it saves?

## 5. Determinism and event sourcing

The engine has no clock and no random numbers: the caller supplies each request's timestamp. So the same sequence of requests always produces byte-identical reports and market data. That property is what makes:

- **testing** possible: the fuzz test can compare against a reference step by step
- **benchmarking** fair: `bench_engine` records requests once and replays them into a fresh engine, timing only the engine
- **recovery** possible in a real exchange: journal every input, and after a crash, replay the journal to rebuild the exact state (event sourcing)

*Questions:* What must an exchange journal to recover after a crash? What breaks determinism in practice (hash map iteration order, uninitialised memory, wall-clock time, threads)? How would you replicate an engine to a hot standby?

## 6. Testing: a naive reference and a fuzzer

`tests/reference_engine.hpp` re-implements every rule as simply as possible. All resting orders sit in one vector, and to find the next match it scans everything for the best price and then the lowest sequence number. It's slow and obviously correct.

The fuzzer sends 180,000 random requests through both engines, deliberately including invalid prices and sizes, unknown symbols, dead order ids and wrong owners. After **every** request:

1. every report must match, line for line
2. the full priority queue of every touched symbol must match, order by order
3. Phase 1's `BookBuilder`, fed the engine's ITCH output, must agree with the engine's own price levels

Planted bugs are caught within tens of requests, so the test itself is tested.

*Questions:* What kinds of bugs could this miss? Why is a slow, simple reference more valuable than a second fast implementation? How would you shrink a failing random sequence to a minimal reproduction?

## 7. Discrete-event simulation

`MarketSim` doesn't use threads or real time. Each agent schedules its next wake-up, and a priority queue always runs whichever agent is due next. A 6.5-hour day takes half a second and is fully reproducible from its seed.

The fair value follows a geometric random walk (about 2 bp per √second, roughly 3% daily volatility), advanced between events by the elapsed time.

*Questions:* Why use exponential wake-up times (a Poisson process)? What does this model leave out compared with a real market: latency, queue position races, news? How would you add network latency between agents and the exchange?

## 8. Market microstructure: who makes money

- **Market makers** quote both sides around their estimate of fair value and earn the bid-ask spread. They skew both quotes against their inventory (long means quote lower) and stop quoting one side at a position limit.
- **Noise traders** trade for reasons unrelated to price, so on average they pay the spread.
- **Informed traders** know the true fair value and only trade against stale quotes. Each of their trades loses the market maker money: that's **adverse selection**, the main reason spreads can't be arbitrarily tight.

Every trade has two agents on it, so PnL across all agents is exactly zero. The sim checks this in integers.

*Questions:* What sets the equilibrium spread (Glosten-Milgrom)? Why do market makers skew quotes with inventory (Avellaneda-Stoikov)? If the informed traders got faster, what would market makers do?
