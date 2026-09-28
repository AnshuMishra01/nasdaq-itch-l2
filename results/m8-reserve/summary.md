# Reserve-size experiment: does reserving price-level capacity kill the tail on creates?

**Prediction:** `reserve()` kills the tail on adds that create a price level, and leaves p50 alone.

**Setup:**
- `Book::reserve_on_first_use = N`: the first time a book side gets a level, its vector reserves N levels. N = 0 is plain `std::vector` growth (the baseline).
- One process per variant, fenced clock, 3 probed + 3 unprobed runs each. State hash checked on every run: all correct.
- The whole sweep ran twice: forward (0 → 64), then reverse (64 → 0), so drift across the session can't favour one end.
- Raw files are in `fwd/reserve_N/` and `rev/reserve_N/` (`latency.md`, `run1-3.md`, `key_metrics.md`).

Reproduce one variant:
```sh
./build/latency_bench.exe data/12302019.NASDAQ_ITCH50.itch50.sample 3 results/m8-reserve/fwd/reserve_16 fenced 16
```

## Results

Level-creating adds = A and F messages that created a price level (1,724,898 over 3 runs). All latencies are ns, raw (the fenced timer adds ~40), and are histogram bucket upper bounds.

**Forward pass (0 → 64):**

| reserve | level-creating adds: p50 | p99 | p99.9 | p99.99 | overall p50 | peak working set | book vectors | unprobed ns/msg, median (runs) |
|---|---|---|---|---|---|---|---|---|
| 0 (baseline) | 104 | 609 | 1,987 | 9,746 | 92 | 165.5 MB | 0.4 MB | 78.2 (77.1 / 78.7 / 78.2) |
| 8 | 104 | 464 | 1,474 | 8,207 | 92 | 166.1 MB | 0.9 MB | 80.3 (96.6 / 80.3 / 76.1) |
| 16 | 104 | 480 | 1,474 | 7,181 | 92 | 167.0 MB | 1.7 MB | 77.2 (77.2 / 77.6 / 74.1) |
| 32 | 104 | 512 | 1,603 | 8,207 | 92 | 168.6 MB | 3.3 MB | 76.4 (73.7 / 76.4 / 81.2) |
| 64 | 104 | 448 | 1,410 | 8,720 | 92 | 171.6 MB | 6.5 MB | 77.1 (77.1 / 77.4 / 74.7) |

**Reverse pass (64 → 0):**

| reserve | level-creating adds: p50 | p99 | p99.9 | p99.99 | overall p50 | peak working set | book vectors | unprobed ns/msg, median (runs) |
|---|---|---|---|---|---|---|---|---|
| 0 (baseline) | 104 | 769 | 2,051 | 11,798 | 92 | 165.5 MB | 0.4 MB | 76.2 (75.7 / 76.3 / 76.2) |
| 8 | 104 | 464 | 1,538 | 7,951 | 92 | 166.2 MB | 0.9 MB | 75.2 (75.7 / 75.2 / 73.3) |
| 16 | 104 | 464 | 1,410 | 5,899 | 92 | 167.0 MB | 1.7 MB | 76.8 (81.5 / 76.8 / 75.8) |
| 32 | 104 | 464 | 1,474 | 7,181 | 92 | 168.6 MB | 3.3 MB | 75.8 (75.8 / 77.1 / 74.5) |
| 64 | 112 | 496 | 1,474 | 17,441 | 104 | 171.6 MB | 6.5 MB | 83.6 (76.1 / 83.6 / 87.5) |

- **Book vectors** = bytes of vector capacity at the end of a run. 8,810 book sides were touched in this file.
- **Peak working set** is the whole process: the 100 MB file buffer, the 48 MB order store, histograms and books.
- **This session ran slower overall than earlier ones** (overall p50 92 here vs 84 before; unprobed 75–80 vs 67–69). Compare rows within this file only.

## Prediction vs result

**"Leaves p50 alone": right.**
Level-creating adds are at 104 ns and all book messages at 92 ns in 9 of 10 variants. The exception, reserve 64 in the reverse pass, is one slow process: its unprobed throughput was also the worst (83.6), so the whole run was slow, not the reserve.

**"Kills the tail on creates": wrong. It trims it.** Level-creating adds, both passes:

| | reserve 0 | any reserve (8–64) | change |
|---|---|---|---|
| p99 | 609 / 769 | 448–512 | **−25 to −40%** |
| p99.9 | 1,987 / 2,051 | 1,410–1,603 | **−20 to −30%** |
| p99.99 | 9,746 / 11,798 | 5,899–8,720 (one outlier at 17,441) | lower, but noisy; not gone |

**Why the tail isn't gone:**
- Reserving removes the *re*-allocations as a side grows (a vector growing from empty reallocates at 1, 2, 4, 8, 16 … levels).
- But the first touch of each side still allocates: 8,810 allocations per run, one per side.
- And the rest of p99.99 is cache misses and OS noise, as the M8 analysis found.
- Removing allocation from message processing completely needs memory set up before the run (e.g. a pool, or fixed arrays per book), not a lazy reserve.

**Reserve 8 already gets nearly all of the gain.** Beyond 8, no size is consistently better: 8, 16, 32 and 64 swap places between the passes, and the differences are within the run-to-run spread. Books in this pre-market file are small (most updates are within a few levels of the best price), so 8 slots covers most sides without ever growing.

**Memory: small.** Book vectors go from 0.4 MB (0) to 0.9 (8), 1.7 (16), 3.3 (32) and 6.5 MB (64). That's 12 bytes × N × 8,810 sides, since every side gets its N slots whether it needs them or not. Peak working set rises by exactly that. A full day touches a similar number of sides (one or two per listed symbol), so this should hold, but it hasn't been measured on full-day data.

**Throughput: no measurable change.** Unprobed medians of 75–80 ns/msg across variants are within the run-to-run spread (a single run varied from 73.7 to 96.6).

## Conclusion

A lazy reserve is a cheap, safe improvement to the p99 and p99.9 of level-creating adds: about −30% for under 1–2 MB at N = 8 or 16. It doesn't change p50 or throughput. It does **not** remove the extreme tail, because the first allocation on each side remains.

## Decision: reserve 16, then stop optimising the books

`Book::reserve_on_first_use = 16` is now the default (`src/core/book.h`). Why 16:
- **Tail numbers:** consistently at or near the best of the sweep.
- **Memory:** 1.7 MB for the book vectors.
- **Headroom:** it covers the typical book depth measured earlier (about 9 levels at p90) with room to spare.
- **Nothing larger helps measurably.**
- **Correctness:** unchanged. The state hash still matches `96fd73902096c354`.

**What the prediction got wrong.** The M8 analysis showed adds that force a vector reallocation reaching p99.9 = 17 µs, against 0.7 µs for adds that don't. The prediction was that reserving would bring level-creating adds down to roughly that 0.7 µs. The measured improvement was about 30%. **Vector regrowth was *a* cause of the tail, not *the* cause.**

**The residual is most likely the things a reserve can't touch:**
- **The first allocation per book side.** That's 8,810 per run, identical at every reserve size. When that allocation needs memory the process hasn't used yet, the kernel has to map and zero a fresh page (a page fault).
- **OS preemption and scheduler jitter.**

Both are independent of the reserve size, which is why 8 ≈ 16 ≈ 32 ≈ 64. (Page faults are the likely explanation but weren't measured per message here.)

**Lazy reservation works as intended:** book memory is 0.4 MB at baseline and 1.7 MB at 16. Only 8,810 of the 131,072 book sides (65,536 books × 2) are ever touched in this file.

**Stopping here.** The remaining tail is page faults and the OS. The fixes for those are systems tuning, not data-structure work, and a Windows laptop can't control the environment well enough for those numbers to mean much. They're listed in the README under "not done, and why".
