# ITCH Feed Handler + Book Builder

A C++23 market-data feed handler for **Nasdaq TotalView-ITCH 5.0**. It reads the exchange's binary message stream, decodes it, tracks every live order, and rebuilds a per-instrument order book (price levels, best bid/ask), as a trading firm's market-data layer does.

This is the **participant** side of an exchange: Nasdaq decides, and this program keeps a copy of what it decided. The copy can be wrong, so correctness checks and honest measurement matter as much as speed.

> Status: **M5 of 10 done.** Built one milestone at a time. Each milestone ends with a measurement or a correctness check, not just code.

---

## What it does today

```
 .itch file ──► framing ──► decode ──► dispatch ──► BookHandler
 (big-endian,   (2-byte     (memcpy +   (switch on    ├─ order store: ref# → {price, shares, locate, side}
  unaligned)     length)     byteswap)   type byte)   └─ books[locate]: bids / asks price levels
```

- **Framing:** walks the length-prefixed stream and checks each message's length against the size the spec gives for its type.
- **Decoding:** one struct per message type, decoded once with `memcpy` + `std::byteswap`. There is no `reinterpret_cast` over the buffer (that would be undefined behaviour: strict aliasing and alignment).
- **Dispatch:** the handler is a template parameter, so dispatch is resolved at compile time. `R A F E C X D U` are handled.
- **Order store:** maps an order reference number to its order. Currently a `std::unordered_map`; it gets replaced in M9.
- **Books:** a `std::vector<Book>` of 65,536 entries indexed directly by stock locate. Each side is a sorted `std::vector<PriceLevel>` with the best price at `back()`.
- **Anomaly counters:** every "impossible" event is counted, not ignored: unknown order refs, over-execution, over-cancel, a missing price level, a level going below zero, a crossed book.

---

## Build and run

The toolchain is MSYS2 UCRT64: g++ with C++23, CMake 3.28 or later, and Ninja.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# itch_count <file> <count> <out> [mode]
#   count <= 0     process the whole file
#   out   = -      print to stdout, otherwise a file path (default metrics.txt)
#   mode           book | stats | print | both (default both)
./build/itch_count.exe data/12302019.NASDAQ_ITCH50.itch50.sample 0 - book
```

To run from a plain shell, put `C:\msys64\ucrt64\bin` on `PATH`, or the executable can't find its runtime DLLs and exits with code 127.

> **Check the optimisation flags.** At one point the CMake cache had an empty `CMAKE_CXX_FLAGS_RELEASE`, so every "Release" build was quietly unoptimised. Confirm that `build/CMakeCache.txt` shows `-O3 -DNDEBUG` before trusting any timing.

---

## Layout

```
src/
  main.cpp                 file load, framing loop, modes, timing
  decode.h                 read_be<T, N>, per-message decoders, dispatch()
  core/
    message.h              decoded message structs, Side
    order.h                Order {price, shares, locate, side}, 12 bytes
    order_store.h          ref# → Order
    book.h                 PriceLevel, Book (bids/asks, add/reduce, crossed)
  handlers/
    book_handler.h         order store + books + anomaly and invariant counters
    combined_handler.h     stats + book in one pass
  measure.h                StatsHandler: counts, ref range, peak live orders
  message_decode.h         PrintHandler (printing removed)
  struct_sizes.h           --dump-structs
nasdaq/                    ITCH 5.0 spec PDF (not committed; download from Nasdaq)
data/                      sample ITCH file (not committed)
NOTES.md                   background: why feed handlers exist, UDP vs TCP, etc.
```

---

## Test data

`data/12302019.NASDAQ_ITCH50.itch50.sample` is a Nasdaq sample file:

- **Size:** 100,921,642 bytes, 3,440,641 messages.
- **Coverage:** pre-market only, starting at 04:00 ET. The download is truncated but ends cleanly on a message boundary.
- **Messages handled:** 3,197,545.
- **Peak live orders:** 27,221.
- **Order refs:** 42 to 5,706,488, so about 4× sparse.

---

## Results so far

Ryzen 5 7530U laptop, `-O3 -DNDEBUG`, whole file, timer around the parse loop only (no I/O inside it).

| Configuration | ns/msg |
|---|---|
| Parse + decode + discard (checksum over every field): the floor | ~17 |
| + order store (`unordered_map`) | ~80 |
| + book (new: scan back from the best level, `vector<Book>`) | ~127 (3 runs: 127.2 / 124.1 / 129.3) |
| + book (old: `lower_bound`, `unordered_map<locate, Book>`) | ~178 in an earlier session; the matching new-book run there was ~147 |

**Cost split, estimated by differencing:** floor ≈ 17, order store ≈ 63, book ≈ 47 ns/msg. Stages share the cache, so a profiler would be needed for exact attribution. The order store is the biggest cost, which is why M9 targets it.

**Why the book scans backwards instead of binary searching:** measured on this data, 50% of updates hit the best price level and 92% land within 3 levels of it. Scanning back from `back()` finds most levels in one or two sequential steps.

**Measurement notes:**
- Timings vary about 4% within a session and 10–15% between sessions. Before/after comparisons are always run back to back.
- The earlier "683 ns/msg" baseline came from the unoptimised build described above. I reproduced it at 618–701 ns with no `-O` flag, and it is not used as a baseline.

**Correctness (M5):**

| Check | Result |
|---|---|
| Order-store anomalies (7 counters) | all 0 |
| Missing price level / level below zero | 0 / 0 |
| Crossed book (best bid ≥ best ask) | 2, both on **PNRL**, which is in the halted state for the whole file. Crossing is legitimate while halted. The check will skip non-trading symbols once `H` is handled (M6). |
| AAPL top 5 levels | bid 289.61 / ask 289.78, levels a few cents apart, sizes 6–1000 shares |

---

## Roadmap

| # | Milestone | What it covers | Status |
|---|---|---|---|
| M0 | Setup | CMake, C++23, strict warnings, sample data and spec | ✅ |
| M1 | Framing | Walk the length-prefixed stream; check sizes per type; clean end of file | ✅ |
| M2 | Decoding | `read_be<T, N>` (memcpy + byteswap), 6-byte timestamps, no UB casts | ✅ |
| M3 | Typed messages + dispatch | One struct per message; compile-time handler; switch on type byte | ✅ |
| M4 | Order store | ref# → order; the 7 anomaly counters, all zero | ✅ |
| M5 | Per-instrument books | Price levels per locate; missing-level, below-zero and crossed checks; eyeball AAPL | ✅ |
| **M6** | **Apply events + edge cases** | Trading state (`H`) so halted books are skipped by the crossed check; system events (`S`); remaining message types (`P Q B I N` etc.) handled explicitly; replace/execute edge cases | ⏭ next |
| M7 | Correctness harness | Replay the whole file and compare against an independent source, e.g. a slow reference book or published data; reproducible pass/fail | ⏳ |
| M8 | Benchmark percentiles | Per-message latency histogram: p50 / p99 / p99.9 / p99.99 / max, not just the average; the method written down | ⏳ |
| M9 | Optimise | Replace `unordered_map` with a flat, open-addressing table (or direct indexing); measure before and after in the same session | ⏳ |
| M10 | Gaps + write-up | Detect sequence gaps and stop trusting a stale book; finish this README with an honest list of what was and wasn't done | ⏳ |

---

## Deliberately out of scope

- Live multicast, UDP networking, colocation. Input is historical files: the same bytes, saved to disk.
- Kernel bypass, FPGA, lock-free or multithreaded pipelines.
- Order entry (OUCH/FIX), strategy, risk.

The goal is a correct, measured book builder with numbers that can be defended.
