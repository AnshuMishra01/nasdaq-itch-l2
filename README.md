# nasdaq-itch-l2

A single-threaded C++23 feed handler that reads Nasdaq TotalView-ITCH 5.0 market data and rebuilds the full order book for every listed stock. Over a complete trading day (30 Dec 2019: 263M book messages, up to 1.9M orders live at once), it processes **~218 ns per book message, about 4.6M messages a second**, excluding disk reads. That's the median of 5 runs on a Ryzen 5 7530U laptop. A typical message takes 168 ns and 99.9% finish within 2.4 µs (timer included). Every number here comes with the conditions it was measured under, and the correctness checks come first: a fast book that's wrong is worth nothing.

---

## What it does

Nasdaq publishes every change to its order book as a stream of small binary messages: an order was added, part of it traded, it was cancelled, it was replaced at a new price. Nasdaq doesn't publish the book itself. A trading firm rebuilds it from the messages, and this program does the same.

```
 ITCH file ─► chunked ─► framing ─► decode ─► dispatch ─► order store ─► per-stock books
              read        2-byte     big-endian switch on  ref# → order    bids / asks as
              64 MB at    length,    fields via the type   (flat hash      sorted price
              a time      size check memcpy +   byte       table)          levels
                                     byteswap
```

- **Reading:** the file is read 64 MB at a time, so memory stays at ~250 MB whether the file is 100 MB or 8 GB. A message split across two chunks is carried over to the next one.
- **Framing** checks every message's length against the size the ITCH 5.0 spec gives for its type.
- **Decoding** reads each field with `memcpy` + `std::byteswap` into one struct per message type, with no `reinterpret_cast` over unaligned bytes.
- **The order store** maps an order reference number to its price, shares, side and stock. Executions, cancels and deletes carry only the reference number, so every one of them needs a lookup here. That makes it the hot path.
- **The books** are one per stock, indexed directly by Nasdaq's stock-locate number. Each side is a sorted vector of price levels with the best price at the back.

It reads **historical files**, not a live feed. Nasdaq sends live data as UDP multicast, which you can only receive inside their data centre (colocation). The files contain the same bytes, recorded.

**Data:**

| | full day (main dataset) | pre-market sample (used early on) |
|---|---|---|
| file | `12302019.NASDAQ_ITCH50` from Nasdaq, 8,251,407,909 bytes decompressed | the first 100.9 MB of the same day |
| messages / book messages | 268,744,780 / **263,241,937** | 3,440,641 / 3,197,545 |
| time covered | 03:04:32 → 20:05:00 | 03:04:32 → 08:14:49 (before the 09:30 open) |
| peak live orders | **1,924,078** | 27,221 |
| price levels at the busiest point | ~754,000 | ~23,000 |

**The pre-market sample turned out to be a poor stand-in for a real day.** It has 70× fewer live orders, and the data structure that won on it was the worst choice at full scale. Every headline number below is from the full day unless it says otherwise.

### What the book looks like

`itch_count <file> 0 - bbo AAPL 10:00:00 100` replays the full day. It prints AAPL's top five levels as of 10:00:00, and every change to its best bid and offer (BBO) in the next 100 ms:

```
Top of book AAPL at 10:00:00.000 (best ask and best bid next to the line):
  ask    286.9100      100 shares    1 orders
  ask    286.9000      100 shares    1 orders
  ask    286.8900      100 shares    1 orders
  ask    286.8700      100 shares    1 orders
  ask    286.8600        1 shares    1 orders
  ----------------------------------------
  bid    286.8300      100 shares    1 orders
  bid    286.8200      100 shares    1 orders
  bid    286.8100      200 shares    2 orders
  bid    286.8000      100 shares    1 orders
  bid    286.7900      400 shares    3 orders

BBO changes for AAPL from 10:00:00.000 to 10:00:00.100 (5):
  time                     bid x shares          ask x shares   spread
  10:00:00.009831422    286.8300 x    100     286.8600 x      3   3.00c
  10:00:00.010389248    286.8300 x    100     286.8600 x    103   3.00c
  10:00:00.010826821    286.8200 x    100     286.8600 x    103   4.00c
  10:00:00.011141305    286.8200 x    100     286.8600 x    102   4.00c
  10:00:00.060897723    286.8100 x    100     286.8600 x    102   5.00c
```

**Something that uses the book.** The same mode tracks every stock's BBO through the day:
- **124.8M BBO changes** (best price or its size) across 8,892 stocks.
- **The most active** are QQQ (1.36M changes), IWM, SPY and TQQQ.
- **Time-weighted quoted spreads** during regular hours are 1.0–1.2 cents for the busiest ETFs and large caps, and **2.64 cents for AAPL**.

These are spreads in **Nasdaq's own book**, not the consolidated quote across all exchanges (the NBBO), which by definition is never wider. Full output: `results/fullday/bbo_AAPL_1000.txt`.

---

## Correctness first

Full write-up: [`results/fullday/correctness.md`](results/fullday/correctness.md).

**Every message is checked against the spec.** All 268,744,780 messages are framed, each one's length must match its type's size in the spec, and the file must end on a message boundary. An unknown type or a wrong size stops the run. The full day passes.
- **First contact with real bytes:** C (execute with price), Q (cross), I (imbalance) and J (auction collar) had never run against real data before.
- **Never seen:** B, N, W and h don't occur in this file, so their sizes are checked only against the spec.

**Nothing "impossible" is silently ignored.** Seven counters record what shouldn't happen on a clean feed: an execute, cancel, delete or replace for an unknown order; an add for an order that already exists; over-executing or over-cancelling. Two more catch a missing price level or a level going below zero, and a tenth catches a book crossed (best bid ≥ best ask) while its stock is trading. **All ten are zero over the full day.**

**The order store and the books check each other.** The same messages update the order store (per order) and the price levels (per price) by different paths. So the shares of all live orders at a price must add up to that level's shares, and their count must equal its order count.
- `verify` mode rebuilds every level from the store and compares, every 500,000 messages. **It passes at all 528 checkpoints on the full day** (up to 1.79M orders against 754k levels).
- **At the end of the day both structures are empty: 0 orders, 0 levels.** Every order added during the day was removed exactly once.
- **Planted bugs prove it can fail.** On the sample, "a partial execution doesn't update the book" gave 728 mismatches, and "a partial fill decrements the order count" gave 494. None of the counters above would have caught the second one.

**Checked against the outside world.** The program reads each stock's closing price from its **closing-cross `Q` message**: Nasdaq's 16:00 closing auction, which is the official close for Nasdaq-listed stocks. That's not the last trade; AAPL last traded at 291.60 after hours. I compared five closes with Yahoo Finance's daily data for 30 Dec 2019:

| | AAPL | MSFT | AMZN | INTC | CSCO |
|---|---|---|---|---|---|
| ours (closing cross) | 291.52 | 157.59 | 1,846.89 | 59.62 | 47.59 |
| Yahoo close × later splits (4:1, —, 20:1, —, —) | 291.52 | 157.59 | 1,846.89 | 59.62 | 47.59 |

- **All five match to the cent.** This is the one check that tests the program against something it didn't produce itself: decoding, price scaling and message selection all have to be right.
- **The opening prices also match for four of the five.** AAPL's is 2 cents off (289.44 vs 289.46), probably because the two sources define "open" differently, but I haven't checked.
- The raw source data is saved in `results/fullday/external/`.

**Unit tests.**
- **Decoders** (`tests/decode_tests.cpp`): every message type built byte by byte, including the largest 6-byte timestamp, the largest 64-bit reference, an unpadded 8-letter symbol and a zero price. Also the trading-action (`H`) decoder, dispatch, book and histogram tests: 96 checks.
- **Order stores** (`tests/store_tests.cpp`): every variant against `std::unordered_map` over 200,000 random operations, plus hand-built cases. The random test **missed a planted wrap-around bug in the delete logic**; a hand-built test for that case now catches it.
- **Reading modes** (`tests/io_tests.cpp`): load, mmap and chunked must deliver identical messages, with chunks as small as 1 byte. A planted "drop the split message" bug fails 14 checks.
- **The sample file** is verified once per reading mode against its golden state hash.

**A state hash.** One 64-bit hash covers the books, the order store and the trade data: `5cdfcc85f1d6976a` for the full day.
- The order-store part is a sum of per-order hashes, so it doesn't depend on how the hash table lays out its entries.
- That made store changes safe to check: every variant tested produced the same hash, including all 8 full-day latency runs across two hash functions and two table sizes.

**No crossed book while trading.** 4,777 updates leave a book with best bid ≥ best ask, and every one of them is accounted for:

| crossed updates | count | why it's allowed |
|---|---|---|
| stock halted, paused or quotation-only (its last `H` message isn't T) | 4,767 | Nasdaq accepts orders without matching them |
| within 100 ms of a cross that left the book crossed | 10 | see below |
| **stock trading and not settling** | **0** | **must be zero** |

The 10 are all halt **reopening crosses** (MKD three times, MBOT once). Nasdaq publishes the cross and switches the stock to T at the same nanosecond, then spends another 0.2–0.5 ms publishing the cross's executions and deleting unfilled remainders. Until that finishes, the book still holds matched orders and is legitimately crossed.
- **The rule:** "settling" starts at a cross only if it left the book crossed, and ends at the first update that leaves the book uncrossed.
- **It's capped at 100 ms**, so a book that stays crossed while trading can't hide behind it. Every real settle took 0.2–12 ms.

Details are in `results/fullday/book_trading_state.txt`.

**The opening auction, answered from the data.** At 09:30, the book orders that trade in the opening cross are removed by **`C` messages marked non-printable**, stamped at exactly the same nanosecond as that stock's `Q`. They aren't removed by deletes (D) or executions (E), which both look like the answer if you only count messages per second. The visible book supplied only 1.7% of the 19.9M shares crossed; my understanding is that the rest came from auction-only orders that ITCH never shows individually. Details: [`results/fullday/auction.md`](results/fullday/auction.md).

---

## Performance

### How it was measured

| | |
|---|---|
| Machine | Ryzen 5 7530U laptop (Zen 3, 6 cores, 16 MB L3), 16 GB RAM, Windows 11 Home; power and thermal behaviour not controlled |
| Compiler | g++ 16.2.0 (MSYS2 UCRT64), `-std=c++23 -O3 -DNDEBUG`, statically linked |
| Data | the full day above, read in 64 MB chunks. Time blocked in `read()` is measured separately and excluded unless stated. The 8 GB file is too big for the OS file cache alongside everything else, so runs most likely read from the SSD (consistent with the ~550 MB/s measured; not verified directly). |
| Threads | one |
| Runs | stated per table; comparisons are always run back to back in one session |
| Variance | about ±4% between runs in a session on small data; on the full day, 5 runs over two sessions spread 202–228 ns for the full pipeline and 73–107 ns store-only, so the tables give medians and ranges; 10–15% between sessions 

**Two measurement mistakes, caught and corrected:**
- **My first baseline was unoptimised.** It said 683 ns/msg because the CMake cache had empty Release flags. Rebuilding without `-O` reproduced it (618–701 ns), and I threw it away.
- **My first full-day table-size grid was mislabelled.** Its "2^21" column had grown itself to 2^22 mid-run, so it measured 2^22 twice. Those cells were re-run with growth switched off.

### Throughput, full day

Five runs over two sessions (2 on 1 Oct, 3 on 2 Oct; `results/scale/fullday_default24/rep1`–`rep5`):

| stage | ns per book message, median | range over 5 runs | messages/second (median) |
|---|---|---|---|
| reading the file from disk | ~50 (at 510–690 MB/s) | | |
| parse + decode, results discarded (the floor) | 17.5 | 16.8–19.9 | ~57M |
| + order store | 79.1 | 73.2–106.8 | |
| **+ books (the full pipeline)** | **217.5** | **202.7–227.6** | **~4.6M** |
| full pipeline including disk reads | 268.1 | 251.6–284.5 | ~3.7M |
| *order store alone (store-only − floor, per run)* | *61* | *56–87* | |
| *books alone (full − store-only, per run)* | *138* | *121–145* | |

The per-stage costs are differences between runs that share the cache, so treat them as estimates. The one 107 ns store-only run is the outlier; the other four fall between 73 and 80. Now that the order store has been fixed, **the books are the larger cost**.

### Latency per message, full day

Every book message timed on its own: 2 runs, 526M samples, thread pinned to one core at high priority. Values are histogram bucket upper bounds, at most 6.25% high. The **40 ns timer cost is included**, since the CPU's counter only moves in 10 ns steps and a fenced read pair costs 40 ns.

| ns per book message | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| full day, current store | **168** | **416** | **897** | **2,436** | **19,492** | not claimed |
| pre-market sample, for comparison | 84 | 152 | 352 | 641 | 2,693 | not claimed |

- **The max was 5–14 ms in every full-day run.** That's Windows preempting the thread; it varies run to run and says nothing about the code.
- **p99.99 differs by up to 2× between runs**, so read it as "roughly 15–25 µs".
- **Where the reproducible tail comes from (on the sample):** messages that create a new price level, because the level vector sometimes has to allocate. See [`results/m8/ANALYSIS.md`](results/m8/ANALYSIS.md).

---

## The experiments

These are the parts I'd point to if asked what I learned. Each has a question, a prediction written down before measuring, and a result, including the ones that went against me.

### 1. Which hash table for order references? The answer flipped when the data got real.

**Question.** The order store is a flat open-addressing hash table. Which hash function, and what size?

**Pre-market answer** ([`results/m9/DECISION.md`](results/m9/DECISION.md)).
- **Identity hashing (`ref & mask`) beat the textbook Fibonacci hash by 3×** (15 vs 45 ns of store cost). Nasdaq hands out order references in increasing order, so live orders sit next to each other in the table and stay in cache, while Fibonacci deliberately scatters them.
- **The catch, already visible there:** at 41% full the worst identity probe was 210 slots, against 12 for Fibonacci.
- **The table chosen:** 2^21 slots, doubling at 50% full.

**Full day** ([`results/scale/fullday/summary.md`](results/scale/fullday/summary.md)): 1.9M live orders, store only, one run each:

| ns/msg | 2^21 (fixed) | 2^22 | 2^23 | 2^24 | 2^25 |
|---|---|---|---|---|---|
| identity | did not finish (> 8.9 µs) | 618 | 135 | 107 | 90 |
| fibonacci | 340 | 170 | 140 | 129 | 150 |

- **The pre-market choice collapsed.** The 2^21 table had doubled to 2^22 and ran 46% full at the peak. Identity's clusters reached **40,046 slots**, making it 3.6× slower than Fibonacci; at 92% full it didn't finish at all.
- **Given room, identity wins again.** At 2^24 it's faster than Fibonacci at every percentile I could measure reliably:

  | | p50 | p90 | p99 | p99.9 |
  |---|---|---|---|---|
  | identity 2^24 | 168 | 416 | 897 | 2,436 |
  | fibonacci 2^24 | 248 | 496 | 1,090 | 2,821 |

  Neighbouring references still share cache lines and memory pages, even though 46 MB of live orders doesn't fit in the 16 MB L3. My prediction was that losing cache locality would hand the win to Fibonacci, and it didn't.

**Decision.** Identity hashing is ~32% faster than Fibonacci at the median and better through p99.9 at 2^24 slots, but it degrades badly with load:
- at 23% full its p99.9 is 2.2× worse than Fibonacci's,
- at 46% it's 3.6× slower on average,
- at 92% it doesn't finish.

So it **ships with a guard**: the table starts at 2^24 slots (384 MB), **8.7× the observed peak**, and doubles once it's 25% full. If the order count were unpredictable, Fibonacci would be the safer default: slower, but its worst probe stays bounded at any load (31 slots at 46% full, 890 at 92%).

### 2. Deleting from the hash table: tombstones or backward shift?

**Prediction:** backward shift (pull later entries back into the hole) beats tombstones (mark the slot deleted) on a workload where almost every order is eventually deleted.

**Result** (pre-market): backward shift was 4–5 ns faster in every session, and tombstones needed a full rebuild once they piled up. As predicted. **Not re-measured on the full day.**

### 3. Reserving price-level capacity: I predicted a 25× improvement and got 30%

**Question.** On the sample, the latency tail was dominated by adds that create a new price level. When those forced the level vector to reallocate, p99.9 was 17 µs, against 0.7 µs when they didn't.

**Prediction:** reserve capacity on first use and those adds should drop from ~17 µs to ~0.7 µs.

**Result** (pre-market, 0/8/16/32/64 levels, two passes):
- the median didn't move;
- p99 improved 25–40% and p99.9 20–30%: about **30% of the predicted effect**;
- 8, 16, 32 and 64 were indistinguishable.

**Why so much less:** reallocation was *a* cause, not *the* cause. Every book side still allocates on first use, and the rest is OS preemption, likely including page faults, which I didn't measure per message. Reserve 16 is the default ([`results/m8-reserve/summary.md`](results/m8-reserve/summary.md)).

**Not re-measured on the full day**, where books are much deeper. The profiling tool exists (`scale_bench books`) but hasn't been run there yet.

### 4. Reading an 8 GB file on a 16 GB laptop

The program used to read the whole file into one vector, which can't work for 8 GB. I compared three ways of reading on a 4 GB part of the day ([`results/io/summary.md`](results/io/summary.md)):

| | time | memory in use | page faults |
|---|---|---|---|
| memory-mapped | 102–103 s | 3.9 GB | 1.03M, inside the parse loop |
| chunked, 64 MB | 101–106 s | 255 MB | 67k |
| load it all | stopped: the system ran out of memory | | |

**Same speed, so chunked reading is now the default:** a fixed ~250 MB of memory, and disk reads happen between chunks rather than as page faults in the middle of processing a message.

---

## What I didn't do

- **No live network path.** No multicast receive, no A/B feed arbitration, no gap recovery. Input is recorded files.
- **No kernel bypass, FPGA or hardware timestamps.** Timing comes from the CPU's cycle counter.
- **Single-threaded**, with no lock-free claims.
- **No order entry** (OUCH/FIX), strategy or risk.
- **No systems tuning for the tail:** no pre-faulted memory, no large pages for the 384 MB table, no isolated core, no fixed CPU clocks. These are the real fixes for what's left of the tail. On a Windows laptop the environment can't be controlled well enough for the results to mean much.
- **Trading state is handled only as far as the crossed-book check needs.** The handler tracks each stock's `H` state and the settling after each cross, but doesn't model auction mechanics itself.
- **Not re-measured at full scale:** book depth and update rank, the tombstone comparison, the reserve size, and a `verify` run with the new store. The latter was started but stopped when the system ran low on memory; the 8 latency runs confirm the same state hash.
- **One trading day**, on a laptop with uncontrolled power and thermal behaviour. A busier day would push the order store toward its 25% guard; on this day it peaked at 11%.

---

## Build and run

**Toolchain:** MSYS2 UCRT64 on Windows, with g++ ≥ 16 (C++23), CMake ≥ 3.28 and Ninja:

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build --output-on-failure
```

- **Check the optimisation flags.** Confirm that `CMAKE_CXX_FLAGS_RELEASE` in `build/CMakeCache.txt` is `-O3 -DNDEBUG` before trusting any timing.
- **`stdc++exp`:** `itch_count` links against it, which MinGW's libstdc++ needs for `<print>`. It's already in `CMakeLists.txt`.
- **Static linking:** every program is linked statically, so it runs from any shell. Without that, Windows can load an older `libstdc++-6.dll` (Git for Windows ships one), and the program crashes or silently writes nothing.

**Data.** Nasdaq publishes historical TotalView-ITCH 5.0 files as daily `.gz` downloads. This project uses 30 December 2019 (`12302019.NASDAQ_ITCH50.gz`, 3.5 GB). Check it and decompress it into `data/`, which is ignored by git:

```sh
gzip -t data/12302019.NASDAQ_ITCH50.gz
gzip -dc data/12302019.NASDAQ_ITCH50.gz > data/12302019.NASDAQ_ITCH50.itch50     # 8.25 GB
```

**Run:**

```sh
F=data/12302019.NASDAQ_ITCH50.itch50

# rebuild the books; anomaly counters, AAPL's top of book, timing, memory
./build/itch_count.exe $F 0 - book

# use the book: per-stock BBO changes and spreads, plus AAPL's top 5 at 10:00 and its BBO changes for 100 ms
./build/itch_count.exe $F 0 - bbo AAPL 10:00:00 100

# the correctness harness: cross-check every 500k messages, closing prices, state hash
./build/itch_count.exe $F 0 results/fullday/verify.txt verify 5cdfcc85f1d6976a

# full-scale experiments (each writes into results/)
./build/scale_bench.exe $F io 2 results/scale/x                      # read time vs processing
./build/scale_bench.exe $F store1 identity24fixed results/scale/x    # one hash-table variant
./build/scale_bench.exe $F latency1 identity24fixed results/scale/x 2   # per-message percentiles
./build/scale_bench.exe $F books - results/scale/x                   # book depth and update rank

# the 09:30 questions
./build/itch_window.exe $F 09:29:59 09:30:01 10 window.txt           # message counts per 10 ms
./build/open_cross.exe $F                                            # what removes orders at the cross
```

`itch_count` arguments: `<file> <count> <out> [mode] [--io=chunked|mmap|load]`.
- **count:** ≤ 0 means the whole file.
- **out:** `-` means stdout.
- **mode:** `book`, `stats`, `print`, `both`, `verify` or `bbo [SYMBOL] [HH:MM:SS] [window_ms]`.
- **--io:** reading defaults to `chunked`.

---

## Layout

```
src/
  main.cpp               itch_count: run a mode over a file, time it
  parse.h                framing loop (shared by every tool)
  itch_io.h              reading: chunked / mmap / load, memory counters
  decode.h               field decoding, one decoder per message type, dispatch()
  latency.h              cycle-counter clock, log-linear histogram
  core/                  message structs, Order, the order stores, Book
  handlers/              BookHandler (store + books + checks), verify, discard, stats
  tools/                 scale_bench, store_bench, latency_bench, msg_window (itch_window), open_cross
tests/                   decoder, histogram, store and reading-mode tests (ctest)
results/
  fullday/               full-day correctness, closing prices vs public data, the opening auction, BBO output
  scale/fullday/         hash table: 2 hashes × 5 sizes, latency, the decision
  io/                    load vs mmap vs chunked
  m8/  m8-plain/  m8-reserve/   latency method, clock comparison, reserve experiment (pre-market)
  m9/                    the original hash-table experiment (pre-market)
NOTES.md                 background: why firms build feed handlers, why market data is UDP multicast
```
