# Phase 1 design notes

Why the code is built the way it is. Each section ends with questions an interviewer could ask about it. You should be able to answer all of them from your own experiments.

---

## 1. Reading the file: mmap

`MappedFile` maps the whole file read-only and calls `madvise(MADV_SEQUENTIAL)`.

- **Zero copies.** The parser reads straight from the kernel's page cache. With `read()`, every byte is copied into a user buffer first.
- **No boundary logic.** With chunked reads, a message can straddle two buffers. With mmap the whole file is one contiguous array.
- **Read-ahead hint.** `MADV_SEQUENTIAL` makes the kernel prefetch aggressively and drop pages behind us, so a 12 GB file doesn't evict everything else.
- **Prefaulting** (`--warm`) touches one byte per 4 KB page before timing. Otherwise the first run measures your disk, not your code.

*Questions:* What happens on the first access to an mmap'd page? What is a major vs minor page fault? Why can mmap be slower than `read()` for some workloads? What would huge pages change?

## 2. Parsing: templates, not virtual calls

`itch::parse<Handler>()` takes the handler as a template parameter. The compiler sees the concrete type, inlines `on_add()` and the rest, and parse + book update becomes one loop with no indirect calls.

- Fields are read with `memcpy` + `__builtin_bswap*`. That's the only portable way to read unaligned big-endian bytes without undefined behaviour, and it compiles to one `MOVBE` or `MOV`+`BSWAP` instruction.
- The parser never allocates or throws. Bad input is counted in `ParseStats`.
- Prices stay as 4-decimal integers end to end. Floating point can't represent 0.10 exactly, and integer compares are cheaper.

*Questions:* Why is casting a `uint8_t*` to `uint64_t*` and dereferencing it undefined behaviour? What does CRTP give you over virtual functions, and what does it cost (compile time, code size)? How would you handle a new message type Nasdaq adds next year?

## 3. Order book: L3 with intrusive FIFOs

Every resting order is stored (market-by-order), because ITCH messages refer to orders by reference number. Each price level keeps a doubly linked FIFO of its orders, linked by **32-bit indices into a pool**, not pointers:

- Indices stay valid when the pool's vector grows; pointers would dangle.
- 4-byte links keep `Order` at exactly 32 bytes, 2 per cache line (`static_assert`ed).
- The FIFO gives queue position for free, which Phase 5 needs to simulate when *our* orders would get filled.

`OrderPool` is a free list: allocation and release are O(1) and never touch the heap once the pool has reached the day's peak size.

*Questions:* Why is an intrusive list better here than `std::list`? What is the cost of a replace (`U`) for queue priority, and why does the exchange do that? How would you compute "shares ahead of me in the queue" in O(1)?

## 4. Price levels: sorted vector vs std::map

Both containers implement the same interface; `OrderBook<Policy>` picks one at compile time.

- **`MapLevels`**: red-black tree, O(log n), but every level is a separate heap node, so traversal chases pointers.
- **`VectorLevels`**: sorted vector with the **best price at the back**. Searches scan linearly from the back. Real order flow is concentrated near the top of the book, so scans usually stop within a few contiguous, prefetched elements, and inserting or erasing near the top shifts only the few elements behind it.

Measured: vector is about 1.33× faster on the synthetic data. The gap depends on how deep in the book activity happens; measure it on real data.

*Questions:* When would the vector lose badly? What's the worst-case complexity and does it matter? Design a price-indexed array book: what do you do when the price moves outside the array?

## 5. Order lookup: open-addressing hash map

`FlatHashMap` maps order ref → pool index:

- **Linear probing** in one flat array: a lookup is usually a single cache line. `std::unordered_map` allocates a node per entry and chases a pointer.
- **Power-of-two capacity**, so the index is `hash & (cap - 1)`, not a modulo.
- **Fibonacci hashing** (multiply by 2⁶⁴/φ) spreads nearly sequential order refs across the table.
- **Backward-shift deletion**: no tombstones, so lookups don't slow down over a long day.
- **Reserve up front**: a rehash copies the whole table, which shows up as a multi-millisecond latency spike.

Measured: 1.5× faster than `std::unordered_map`. Table size matters even more: a 2 MB table (fits in L2) gave 54.7 ns/msg vs 95.5 ns/msg for a 256 MB table.

*Questions:* Why does linear probing degrade above ~70% load? What's primary clustering? Why does the 256 MB table hurt even though most of it is empty? How would you size it for a real day?

## 6. Measuring: TSC and a log-linear histogram

- `rdtsc` costs ~20 cycles versus ~25 ns for `clock_gettime`. On modern x86 the TSC is invariant (constant rate), so it's calibrated once against `steady_clock`.
- `LatencyHistogram` splits each power of two into 32 buckets: ~3% precision from 1 ns to hours in a fixed 8 KB table. Recording is a few instructions and never allocates. This is the idea behind HdrHistogram.
- Report **percentiles, not averages**. A trading system lives or dies by its tail (p99, p99.9, max).

*Questions:* Why isn't `rdtsc` a serializing instruction, and when does that matter (`rdtscp`, `lfence`)? Why does the mean hide the problems you care about? What causes the multi-microsecond outliers in the histogram?

## 7. Testing: differential, invariants, sanitizers

- **Differential test**: the fast books are compared with a naive reference after every message of synthetic days. The reference is easy to trust because it's simple. A planted one-line bug is caught at message 175.
- **Invariant checker**: walks every FIFO and checks links, totals, ordering and the ref map.
- **Sanitizers**: ASan finds out-of-bounds reads and use-after-free; UBSan finds integer overflow and misaligned access.
- **Synthetic generator**: obeys every exchange rule (valid refs, price-time priority, no crossing), so any error counter that's non-zero is our bug.

*Questions:* What kinds of bugs would the differential test miss? How would you fuzz the parser? Why use a hand-written RNG instead of `std::uniform_int_distribution`?

## 8. Viewer: off the hot path

The C++ replay writes snapshots; a separate Node process streams them over a WebSocket at the chosen speed, throttled to 20 frames/sec. The browser only renders what it receives, and reuses DOM nodes instead of rebuilding them.

This is the same split the live dashboard will use in Phase 4: the trading thread publishes into a lock-free buffer, and a separate process does everything slow (JSON, sockets, rendering).

*Questions:* Why must the trading thread never block on the UI? What happens if the UI process falls behind: drop or queue? How would you send a kill-switch command back safely?
