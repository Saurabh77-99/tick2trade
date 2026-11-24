# HFT Stack

A low-latency trading stack built from scratch in C++20, one layer at a time:

- **Phase 1 (market data):** a Nasdaq TotalView-ITCH 5.0 parser and a full-depth (L3) order book.
- **Phase 2 (exchange):** a price-time priority matching engine that publishes its own ITCH feed, plus an agent-based market simulation of market makers, noise traders and informed traders trading against it.

Every component has tests, a benchmark, and design notes. A web viewer replays any order book.

![viewer](docs/viewer.png)

## What's inside

| Path | What it is |
|---|---|
| `include/hft/itch/` | ITCH 5.0 message definitions, zero-copy parser, encoder |
| `include/hft/book/` | L3 order book: order pool, per-level FIFO queues, two price-level containers |
| `include/hft/exchange/` | Order entry types and the **matching engine** |
| `include/hft/sim/` | Reproducible RNG, synthetic feed generator, **agent-based market simulation** |
| `include/hft/common/` | Big-endian loads, mmap file, open-addressing hash map, TSC timing, latency histogram |
| `src/tools/` | The `itch` CLI: `gen`, `sim`, `stats`, `replay`, `snapshot` |
| `tests/` | GoogleTest: unit tests, two **differential fuzz tests** against naive reference implementations |
| `bench/` | `bench_book` (data structures, feed replay) and `bench_engine` (matching engine latency) |
| `viewer/` | Node WebSocket server + browser UI (depth ladder, mid chart, trade tape) |
| `docs/` | Design notes for each phase, with the interview questions each one prepares you for |

## Quick start

You need Linux or macOS (on Windows, use **WSL2 with Ubuntu**), a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.20+, Node 18+, and git.

```bash
# Ubuntu / WSL2
sudo apt install build-essential cmake git nodejs npm

bash scripts/demo.sh     # build, test, run both phases, start the viewer
# then open http://localhost:8080
```

Or step by step:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Phase 1: parse a feed and rebuild the book
./build/itch gen    data/synthetic.itch --messages 5000000 --symbols 12
./build/itch replay data/synthetic.itch --symbol AAPL --depth 5 --check
./build/bench_book

# Phase 2: run a simulated exchange, then rebuild its book from its own feed
./build/itch sim    data/sim.itch
./build/itch replay data/sim.itch --check
./build/bench_engine

# Viewer
./build/itch snapshot data/sim.itch --symbol AAPL --out data/AAPL.jsonl
cd viewer && npm install && node server.js ../data/AAPL.jsonl
```

Viewer keys: **space** play/pause, **← / →** step one snapshot (hold **shift** for 60), **1–5** speed.

## Phase 2: the exchange

`MatchingEngine` is a continuous limit order book, like the core of a real exchange:

- **Price-time priority.** Best price first; at the same price, first come first served. Trades execute at the resting order's price.
- **Order types:** limit and market; time in force DAY, IOC (immediate-or-cancel) and FOK (fill-or-kill); post-only (never takes liquidity).
- **Cancel and modify.** Shrinking an order keeps its place in the queue. Growing it or changing its price sends it to the back with a new id. A modify that crosses the spread trades immediately.
- **Self-trade prevention.** An order never trades with another order from the same trader; the resting one is canceled instead.
- **Two outputs.** Private reports to each trader (accepted / rejected / fill / canceled / replaced), and a public, anonymous **ITCH 5.0 market data feed**.
- **Deterministic.** No clock and no randomness inside. The same input gives byte-identical output, so any run can be replayed exactly.

`itch sim` puts agents in front of the engine and runs a 6.5-hour trading day in about half a second:

```
  agent        type                 orders       shares      fills        PnL ($)
  all x2       informed trader       26428      6740000      31990      419117.28
  all x3       market maker          23690     10536200      47257      607085.10
  all x30      noise trader         135043     33377000     171107    -1026202.38

  sum of all PnL: -0.00   zero-sum check (exact): OK
```

Market makers earn the spread, informed traders profit from stale quotes, and noise traders pay for both. Since every buyer and seller is an agent, total PnL must be exactly zero; the sim checks this using integer arithmetic.

## Running on a real Nasdaq trading day

Nasdaq publishes free sample days at `https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`, for example `01302019.NASDAQ_ITCH50.gz` (about 4.7 GB compressed, 12+ GB uncompressed). On WSL2, keep it inside the Linux filesystem (`~/itch-data/`), not on `C:`, which WSL reads slowly.

```bash
mkdir -p ~/itch-data && cd ~/itch-data
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
gunzip 01302019.NASDAQ_ITCH50.gz
cd -
./build/itch stats  ~/itch-data/01302019.NASDAQ_ITCH50
./build/itch replay ~/itch-data/01302019.NASDAQ_ITCH50 --warm --symbol AAPL --check
./build/bench_book --file ~/itch-data/01302019.NASDAQ_ITCH50
```

On a complete day, `checks: unknown refs` must be 0; anything else is a bug. `crossed after add` can legitimately be non-zero around auctions and halts; finding out when it happens is a good exercise.

## Commands

```
itch gen      <out.itch> [--symbols 8] [--messages 2000000] [--seed 42]
itch sim      <out.itch> [--symbols 4] [--mm 3] [--noise 30] [--informed 2] [--hours 6.5] [--seed 7]
itch stats    <file.itch>
itch replay   <file.itch> [--book vector|map] [--symbol AAPL] [--depth 10]
                          [--latency] [--warm] [--check] [--reserve 65536]
itch snapshot <file.itch> --symbol AAPL [--interval-ms 1000] [--depth 10]
                          [--from 09:30] [--to 16:00] [--out AAPL.jsonl]
bench_book    [--messages 5000000] [--symbols 8] [--file real.itch] [--runs 3]
bench_engine  [--requests 2000000] [--symbols 4] [--runs 3]
```

## Results

Measured on a 2-vCPU cloud VM (Intel Xeon @ 2.1 GHz, 4 MB L2). **Your numbers will differ. Run it yourself and record your own.** On a Windows laptop under WSL2, the Phase 1 replay ran at 45.6 ns/msg (21.9 M msg/s).

**Phase 1: feed parsing and book building** (synthetic day, 5 M messages, 12 symbols)

| Measurement | Result |
|---|---|
| Parse only (decode every message, no book) | **12.8 ns/msg**, 78 M msg/s, 2.6 GB/s |
| Parse + book, vector levels | **48.5 ns/msg**, 20.6 M msg/s |
| Parse + book, `std::map` levels | 64.6 ns/msg (1.33× slower) |
| Order map: `FlatHashMap` vs `std::unordered_map` | 25.3 vs 38.0 ns/op (1.5× faster) |
| Per-message latency, vector book (incl. ~10 ns rdtsc cost) | p50 68 ns · p99 194 ns · p99.9 388 ns |

The hash table's size alone changes throughput by almost 2×:

| `--reserve` | Table size | ns/msg |
|---|---|---|
| 65,536 | 2 MB (fits in L2) | 54.7 |
| 1,048,576 | 32 MB | 77.2 |
| 8,388,608 | 256 MB | 95.5 |

**Phase 2: matching engine** (2 M requests: 45% passive orders, 10% aggressive IOC, 30% cancels, 15% modifies)

| Configuration | ns/request | M requests/s | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| Vector book | **62.9** | **15.9** | 81 ns | 312 ns | 480 ns |
| Vector book + ITCH feed | 87.6 | 11.4 | 87 ns | 449 ns | 1.9 µs |
| `std::map` book | 86.9 | 11.5 | 87 ns | 320 ns | 502 ns |

## Correctness strategy

1. **Unit tests** for every component, including hand-written scenarios that read like a spec of the matching rules.
2. **Differential testing, book.** Synthetic days are replayed through the vector book, the map book and a naive reference; the top 10 levels are compared after every message.
3. **Differential fuzzing, engine.** 180,000 random requests (all order types, invalid input, wrong owners, dead ids) go through the matching engine and a deliberately naive reference engine. Every report must match line for line, and the full order queues after every request.
4. **Feed consistency.** During the engine fuzz, Phase 1's `BookBuilder` rebuilds the book from the engine's ITCH output, and it must equal the engine's book after every request. The exchange's public feed is proven to describe its private state exactly.
5. **Testing the tests.** Planted one-line bugs are caught: a book bug at message 175, an engine price bug at op 20, a queue-priority bug at op 32.
6. **Invariant checkers** walk every FIFO and check links, totals, ordering, the id map, and that the book is never crossed.
7. **Sanitizers.** `cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHFT_SANITIZE=ON` builds with AddressSanitizer and UBSan. All tests pass under them.
8. **Zero-sum check** on every simulated day.

## Exercises

Phase 1:
1. Replay a real Nasdaq day. Record throughput and the counters. Which message types dominate?
2. Run `perf stat -e cache-misses,branch-misses ./build/itch replay ...` for both `--book` options. Explain the difference.
3. Add a third price-level container (a flat array indexed by price tick) and benchmark all three.

Phase 2:
4. Add a new order type: **iceberg** orders, which show only part of their size and refill from a hidden reserve. Extend the reference engine first, then make the fuzz test pass.
5. Change self-trade prevention to "cancel the incoming order" instead. What changes in the reference engine?
6. Make the market makers quote tighter (`half_spread_ticks = 0.5`). What happens to their PnL, and why?
7. Find the source of the 1.9 µs p99.9 when the ITCH feed is on. (Hint: the output buffer is `reserve()`d, but has its memory ever been touched? What does the OS do on the first write to a page?)

## Roadmap

- [x] **Phase 1:** ITCH parser, order book, replay, benchmarks, viewer
- [x] **Phase 2:** matching engine, differential fuzz testing, agent-based market simulation
- [ ] **Phase 3:** networking: UDP multicast market data (MoldUDP64-style), TCP order entry (OUCH-style)
- [ ] **Phase 4:** trading engine internals: SPSC queues, pinned threads, async logging, live dashboard
- [ ] **Phase 5:** strategy + backtester: market making, queue position, fill model, PnL
- [ ] **Phase 6:** measure and optimize: tick-to-trade latency histograms, perf, huge pages
