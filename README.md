# nasdaq-itch-l2

A single-threaded C++23 feed handler that reads Nasdaq TotalView-ITCH 5.0 market data and rebuilds the full order book for every listed stock. On a Ryzen 5 7530U laptop it processes a book message in about 70 ns end to end (median of 3 runs over 3.2M messages). The typical message needs about 44 ns of actual work, and 99.9% finish within about 600 ns. Every number in this README comes with the conditions it was measured under. The correctness checks come first, because a fast book that's wrong is worth nothing.

---

## What it does

Nasdaq publishes every change to its order book as a stream of small binary messages: an order was added, part of it traded, it was cancelled, it was replaced at a new price. Nasdaq doesn't publish the book itself. A trading firm rebuilds it from the messages, and this program does the same.

```
 ITCH file ─► framing ─► decode ─► dispatch ─► order store ─► per-stock books
             2-byte       big-endian  switch on   ref# → order    bids / asks as sorted
             length,      fields via  the type    (flat hash      price levels, best
             size check   memcpy +    byte        table)          price at the back
                          byteswap
```

- **Framing** walks the length-prefixed stream and checks every message's length against the size the spec gives for its type.
- **Decoding** reads each field with `memcpy` + `std::byteswap` into one struct per message type. There's no `reinterpret_cast` over the buffer, because the fields are unaligned and that would be undefined behaviour.
- **The order store** maps an order reference number to its price, shares, side and stock. Executions, cancels and deletes carry only the reference number, so every one of them needs a lookup here. That makes it the hot path.
- **The books** are one per stock, indexed directly by Nasdaq's stock-locate number. Each side is a small sorted vector with the best price at the back, because that's where most updates land.

It reads **historical files**, not a live feed. Nasdaq sends live data as UDP multicast, which you can only receive inside their data centre (colocation). The files contain the same bytes, recorded.

The data used throughout is one Nasdaq sample file for 30 December 2019:
- 100,921,642 bytes, **3,440,641 messages**, of which **3,197,545 are book messages** (add, add with MPID, execute, execute with price, cancel, delete, replace).
- It covers **03:04:32 to 08:14:49** Eastern, which is pre-market only, before the 09:30 open.
- At its busiest, 27,221 orders are live at once.

---

## Correctness first

Everything here can be re-run with `ctest` and the `verify` mode (see [Build and run](#build-and-run)).

**Every message is checked against the spec.** All 3,440,641 messages are framed, and each one's length must match the size the ITCH 5.0 spec gives for its type. An unknown type or a wrong size stops the run. The file passes cleanly.

**Nothing "impossible" is silently ignored.** Seven counters record events that shouldn't happen on a clean feed: an execution, cancel, delete or replace for an order we've never seen; an add for an order that already exists; executing or cancelling more shares than an order has. All seven are **zero** on this file. Two book-level counters (a price level that should exist but doesn't, and a level going below zero) are also zero.

A third check, for crossed books (best bid ≥ best ask), reports **2**. Both are on one stock, PNRL, which is halted from the moment it appears (03:09) until the file ends. Nasdaq doesn't match orders while a stock is halted, so its book can legitimately cross. The check will skip halted stocks once trading-state messages are handled.

**The order store and the books check each other.** The same messages update two structures by different paths: the order store (per order) and the price levels (per price). So at any moment, the shares of all live orders at a given price must add up to that level's shares, and their count must equal the level's order count.
- `verify` mode rebuilds every level from the order store and compares, every 500,000 messages and again at the end. It **passes at all 6 checkpoints and at the end** (27,197 orders, 23,162 levels).
- To prove it can fail, I planted two bugs. "A partial execution doesn't update the book" gave 728 share mismatches at the first checkpoint. "A partial fill decrements the order count" gave 494 count mismatches. None of the counters above would have caught the second one.

**Decoder unit tests.** `tests/decode_tests.cpp` hand-builds messages byte by byte, with the nasty cases:
- the largest 6-byte timestamp,
- the largest 64-bit order reference,
- an 8-letter symbol with no padding,
- a price of zero.

It checks every decoded field, plus dispatch routing, the book's add/reduce paths and the latency histogram: 90 checks in all. Reading one price field from the wrong byte fails two of them. The histogram tests also caught a real bug: percentile ranks were rounded down, which quietly dropped the single outlier a p99.99 is meant to catch.

**The random test that missed a bug.** `tests/store_tests.cpp` runs every order-store variant against `std::unordered_map` over 200,000 random operations. When I planted a bug in the delete logic's wrap-around case (a run of entries crossing the end of the table), **the random test passed anyway.** It never produced that exact layout. A hand-built test for that case now catches it. Random tests find a lot, but not the specific corner you're afraid of.

**A state hash.** At the end of a run, one 64-bit hash covers the books, the order store and the trade data: `96fd73902096c354` for this file.
- It's the same in Release and Debug (with bounds checks on), and across repeated runs.
- The order-store part is **order-independent**: a sum of per-order hashes, so the order in which the hash table happens to store entries doesn't matter. That's what made the store rewrite safe to check. All 12 store variants produced the identical hash.
- `ctest` fails if it ever changes.

**Not verified yet.**
- **Against reality:** everything above checks the program against itself. The real test is comparing the closing auction price for a few stocks with the published close. The sample ends at 08:14, before any close, so that waits for the full-day file, which is downloading.
- **The opening auction:** it's still an open question which messages remove matched orders from the book at the 09:30 opening auction. The auction result message (`Q`) names no orders. `itch_window` is ready to count message types around 09:30 once the full day is here.

---

## Performance

### How it was measured

| | |
|---|---|
| Machine | Ryzen 5 7530U laptop (Zen 3, 6 cores), Windows 11 Home; power and thermal behaviour not controlled |
| Compiler | g++ 16.2.0 (MSYS2 UCRT64), `-std=c++23 -O3 -DNDEBUG` |
| Data | the sample file above, loaded into memory before timing; 3,197,545 book messages |
| Threads | one |
| Runs | 3 per configuration, reporting the median; comparisons always run back to back in the same session |
| Variance | about ±4% between runs in one session, 10–15% between sessions (the whole laptop drifts) |

**A mistake worth mentioning.** My first "baseline" was 683 ns/msg. It turned out the CMake cache had empty Release flags, so "Release" was compiling without optimisation. I rebuilt that configuration without `-O` and reproduced 618–701 ns, then threw the number away. Check `CMAKE_CXX_FLAGS_RELEASE` in `build/CMakeCache.txt` before trusting any timing from this repo.

### Throughput

| What | ns per book message | Conditions |
|---|---|---|
| Floor: parse + decode, results discarded | ~17–22 | checksum over every field so the compiler can't delete the work; varies by session |
| Full pipeline, original `std::unordered_map` store | 147–166 | 3 sessions, median of 3 interleaved runs each, same process as the next row |
| Full pipeline, flat hash-table store | **75–82** | same sessions: **about half** |
| Full pipeline, current code, standalone | **67–69** | `itch_count` book mode, 3 runs in one session; the later reserve change made no measurable difference |

**Order store cost**, estimated as (store-only time − floor): **70 → 15 ns/msg**, from 66–70 down to 13.7–14.8 over 3 sessions (−79%). The books add roughly another 40–47 ns. These splits come from subtracting separate runs, which share the cache, so treat them as estimates rather than exact attribution.

### Latency per message

Here every book message is timed on its own with the CPU's cycle counter. 3 runs, 9.6M samples per clock, one thread pinned to one core at high priority. Values are histogram bucket upper bounds, so at most 6.25% high.

| ns per message, timer cost removed | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| fenced clock (`lfence; rdtsc` … `rdtscp; lfence`), timer = 40 ns | **44** | **112** | **312** | **601** | **2,653** | not claimed |
| plain `rdtsc`, timer = 20 ns | 42 | 84 | 236 | 557 | 1,390 | not claimed |

Three things to know before trusting this table:
- **The stopwatch is almost as expensive as the work.** The counter moves in 10 ns steps on this CPU, and a fenced read pair costs 40 ns while the typical message needs ~44. The fences also stop the CPU overlapping consecutive messages, so the whole run slows from ~68 to ~165 ns/msg while it's being measured. These are costs of one message in isolation, not a share of throughput.
- **Two clocks as a cross-check.** The unfenced clock is half as expensive but less exact, because the CPU can move the counter reads around. After subtracting each clock's own cost, the two agree at the median, and the fenced clock shows a heavier tail (p99.99 1.9× in one session; in another they agreed up to p99.9). So I give p99.99 as a range, **~1.4–2.7 µs**, and the fenced figures as the headline.
- **No maximum is claimed.** The worst message varied from about 28 to 488 µs between runs, and the ten slowest messages were almost never the same ones twice. That is Windows and the hardware (interrupts, preemption, power states), not the code, so it would be dishonest to quote it.

The latency numbers were measured before the reserve change described below. That change lowers the tail of adds that create a price level by about 30% at p99.

Full data is in `results/m8/` (fenced) and `results/m8-plain/`, and the write-up is in [`results/m8/ANALYSIS.md`](results/m8/ANALYSIS.md). It includes the tail broken down by message type: adds, deletes and replaces make up about 90% of the slowest 0.1% of messages.

---

## Three experiments

These are the parts I'd point to if asked what I actually learned. Each one has a question, a prediction, a measurement and a conclusion. They're only useful if I report the ones that went against me as well.

### 1. Which hash function for order references? The obvious answer lost.

**Question.** The flat order store needs a hash function. Should it be identity (`ref & mask`), or a multiplicative "Fibonacci" hash that scatters keys evenly?

**Prediction.** The textbook answer is Fibonacci. Identity hashing on nearly consecutive keys invites clustering, which is bad for linear probing.

**Measured.** In a 2M-slot table (48 MB), store cost per message over 3 sessions:
- identity: **13.7–14.8 ns**
- Fibonacci: **39.8–44.5 ns**, about 3× slower.

Yet both find almost every order on the first slot they check (mean probe distance 0.00 vs 0.01). So the difference isn't collisions.

**Why.** Nasdaq hands out order references in increasing order, and the live orders are mostly recent ones. With identity, recent orders sit next to each other in a small, hot region of the table. Fibonacci deliberately spreads them across all 48 MB, so almost every lookup misses the cache. Shrink the table to 1.5 MB so it fits in cache, and the gap disappears (Fibonacci 12.9–14.2 vs identity 14.0–16.2 ns).

**Conclusion.** Identity wins, *for this data and at low load*. The catch shows up when the table fills: at 41% full, identity's worst probe was 210 slots, against 12 for Fibonacci. The chosen table starts at 2M slots, so it stays below 25% full up to ~500k live orders. A full trading day may go well past the 27k peak here, so this is the first thing to re-check on the full-day file.

### 2. Deleting from the table: tombstones or backward shift?

**Question.** Almost every order is eventually deleted (27k live at peak, over a file of 3.2M book messages). How should deletes work?

**Prediction.** Backward shift: pull later entries back into the hole so no "deleted" markers pile up. It should beat tombstones on a workload this delete-heavy.

**Measured.** Store cost 13.7–14.8 ns with backward shift vs **18.7–20.0 ns with tombstones** in every session. The tombstone table also needed a full rebuild once, when the markers piled up.

**Conclusion.** Backward shift, as predicted. The same experiment settled the layout (keys and values stored together vs in separate arrays: a tie, because probes almost never go past the first slot) and the empty marker (key 0 vs a metadata byte: the byte was 0.6–2.4 ns slower in every session). Every variant changed exactly one thing, and all 12 produced the same state hash. Details: [`results/m9/DECISION.md`](results/m9/DECISION.md).

### 3. Reserving price-level capacity: I predicted a collapse and got 30%

**Question.** The latency tail was dominated by adds that create a new price level. Did creating the level itself cause it?

**What pointed there.** In a scratch experiment, adds that forced a book's `std::vector` to grow (ask for a bigger block of memory and copy the levels across) had p99.9 = **17 µs**, against **0.7 µs** for adds that didn't. And the messages that were slow in every run almost all created a price level.

**Prediction.** Reserve capacity the first time a book side is used, and those adds should fall from ~17 µs to ~0.7 µs, roughly 25× better. The median shouldn't move.

**Measured.** Reserve sizes 0, 8, 16, 32 and 64, each in its own process, with the whole sweep run forward and then in reverse:
- **The median didn't move** (104 ns for these adds, 92 ns overall). That part was right.
- **The tail improved by far less than predicted:** p99 fell 25–40%, p99.9 fell 20–30%, and p99.99 dropped but stayed noisy. That's about 30% of what I predicted.
- **Reserve 8, 16, 32 and 64 were indistinguishable.** The same setting varied more between the two passes than the different settings did between each other.

**What the residual turned out to be.** Vector regrowth was *a* cause, not *the* cause. What's left doesn't depend on the reserve size:
- **Every book side still allocates once, on first use.** That's 8,810 sides in this file. When that allocation needs fresh memory, the kernel has to map and zero a new page.
- **The OS still preempts the thread.**

That's why a bigger reserve changes nothing. The page-fault explanation fits the data, but I didn't measure page faults per message.

**Conclusion.** Reserve 16 is now the default: a ~30% better tail on level-creating adds for 1.7 MB of memory, with no change to the median or to throughput. Beyond that, making the tail go away is systems tuning, not data-structure work, so I stopped optimising the books there. Details: [`results/m8-reserve/summary.md`](results/m8-reserve/summary.md).

---

## What I didn't do

- **No live network path.** No multicast receive, no A/B feed arbitration, no gap recovery or retransmission requests. Input is recorded files.
- **No kernel bypass, FPGA or hardware timestamps.** Timing comes from the CPU's cycle counter.
- **Single-threaded.** There's no lock-free pipeline and I make no claims about one.
- **No order entry** (OUCH/FIX), strategy or risk checks.
- **No systems tuning for the tail:**
  - pre-faulting memory, or allocating all book memory before processing starts;
  - large pages for the 48 MB order store;
  - a CPU core the OS never schedules anything else on;
  - fixed CPU clocks.

  These are the real fixes for what's left of the tail. But on a Windows laptop I can't control the environment well enough for the results to mean much, so I've listed them instead of claiming them.
- **Measured on a laptop,** with uncontrolled power and thermal behaviour. That shows up as the 10–15% drift between sessions.
- **Pre-market data only**: about five hours before the open, 27k live orders at peak.

**What the full-day file would change:**
- far more live orders, which tests whether identity hashing stays fast as the table fills;
- deeper books and bursts around the open and close, so probably a heavier latency tail;
- the first real check against the outside world, closing prices vs published data;
- the answer to what removes crossed orders at the 09:30 opening auction.

The download is in progress. Until it finishes, every number here describes the pre-market only.

---

## Build and run

**Toolchain:** MSYS2 UCRT64 on Windows, with g++ ≥ 16 (C++23), CMake ≥ 3.28 and Ninja:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
```

**Build and test:**

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

- **`stdc++exp`:** `itch_count` links against it, which MinGW's libstdc++ needs for `<print>`. It's already in `CMakeLists.txt`. If you build by hand, add `-lstdc++exp`.
- **Exit code 127:** if you run the binaries from a plain shell (not the MSYS2 UCRT64 terminal), put `C:\msys64\ucrt64\bin` on `PATH`. Otherwise they can't find their runtime DLLs and exit with this code.

**Data.** Nasdaq publishes historical TotalView-ITCH 5.0 files for download (daily `.gz` files). This repo uses the 30 December 2019 file (`12302019.NASDAQ_ITCH50`). The sample used here is its first 100.9 MB, decompressed. Put files under `data/`; the folder is ignored by git.

**Run:**

```sh
F=data/12302019.NASDAQ_ITCH50.itch50.sample

# rebuild the books; prints anomaly counters, AAPL's top 5 levels and timing
./build/itch_count.exe $F 0 - book

# the correctness harness: store-vs-book cross-check, trade prices, state hash
# (exit code 2 if the hash differs from the one given)
./build/itch_count.exe $F 0 - verify 96fd73902096c354

# the experiments: each writes its results into results/
./build/store_bench.exe   $F 3 results/m9                    # order-store variants
./build/latency_bench.exe $F 3 results/m8 fenced             # per-message percentiles
./build/latency_bench.exe $F 3 results/m8-reserve/r16 fenced 16

# message counts by type per time bucket (reads the file in chunks; built for full-day files)
./build/itch_window.exe data/<full-day file> 09:29:59 09:30:01 10
```

`itch_count` arguments: `<file> <count> <out> [mode]`. A count ≤ 0 means the whole file; an out of `-` means stdout; the mode is `book`, `stats`, `print`, `both` or `verify`.

---

## Layout

```
src/
  main.cpp               itch_count: load, run a mode, time it
  parse.h                framing loop (shared by every tool)
  decode.h               read_be<T, N>, one decoder per message type, dispatch()
  latency.h              cycle-counter clock, log-linear histogram
  core/                  message structs, Order, the order stores, Book
  handlers/              BookHandler (store + books + checks), verify, discard, stats
  tools/                 store_bench, latency_bench, msg_window (itch_window)
tests/                   decoder, histogram and order-store tests (ctest)
results/                 every experiment's raw output and written conclusions
  m8/  m8-plain/         latency percentiles, fenced and plain clock
  m8-reserve/            the reserve-size experiment
  m9/                    the order-store experiment
NOTES.md                 background: why firms build feed handlers, why market data is UDP multicast
```
