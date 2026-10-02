# Order store at full-day scale: identity vs Fibonacci, 2^21 to 2^25 slots

**Data:** the complete 30 Dec 2019 file (`data/12302019.NASDAQ_ITCH50.itch50`, 8.25 GB, 268.7M messages, 263.2M book messages). **Peak live orders 1,924,078**, against 27,221 in the pre-market sample.

**Correction to the first version of this file.** Its "2^21" column was a table that *started* at 2^21 and doubled to 2^22 once it passed 50% full (1 rehash). So it measured 2^22 twice, and its higher memory came from holding both tables during the rehash. Those runs are kept as `store1_*21_GREW_to_2^22.md`. The 2^21 cells below come from **fixed-size** tables (growth switched off, `FlatStore<..., Grow = false>`). Every row now reports its final capacity and rehash count; all are 0 rehashes. The 2^22 cells were valid all along (0 rehashes) and are unchanged.

## Store only

**Method:**
- `scale_bench <file> store1 <variant>`: one process per variant, one run each, back to back in one session.
- **ns/msg** = parse time (framing + decode + store) per book message. Read time (~45 ns/msg) and snapshot time are excluded. The parse + decode floor is ~24 ns/msg, so store cost ≈ ns/msg − 24.
- **Probes** were snapshotted every 8M messages: "mean at peak" is the snapshot with the most live orders, and "worst" is the largest at any snapshot.
- Ryzen 5 7530U laptop, Windows 11, g++ 16.2 `-O3`. With single runs, differences under ~5% are noise.

**ns per message**

| | 2^21 | 2^22 | 2^23 | 2^24 | 2^25 |
|---|---|---|---|---|---|
| **identity** | **did not finish** (> 8,900) | 618.2 | 134.8 | 106.9 | **90.0** |
| **fibonacci** | 340.2 | 170.3 | 139.6 | 129.2 | 149.5 |

**Load factor at peak** (the same for both hashes): 2^21 **0.92** · 2^22 0.46 · 2^23 0.23 · 2^24 0.11 · 2^25 0.06

**Average probe distance at peak**

| | 2^21 | 2^22 | 2^23 | 2^24 | 2^25 |
|---|---|---|---|---|---|
| identity | — | 7.34 | 0.28 | 0.05 | 0.03 |
| fibonacci | 4.34 | 0.35 | 0.11 | 0.05 | 0.02 |

**Worst probe distance, any snapshot**

| | 2^21 | 2^22 | 2^23 | 2^24 | 2^25 |
|---|---|---|---|---|---|
| identity | — | **40,046** | **13,253** | 248 | 229 |
| fibonacci | 890 | 31 | 11 | 7 | 5 |

**Peak private memory, MB** (table + 64 MB read buffer + ~7 MB of handler arrays; the same for both hashes): 2^21 119 · 2^22 167 · 2^23 263 · 2^24 455 · 2^25 840

- **Identity at a fixed 2^21 (92% full) didn't finish.** I stopped it after ~39 minutes. It can't have averaged less than ~8.9 µs per message (39 min over 263.2M book messages), which is at least 26× Fibonacci at the same size (340 ns, done in ~2 minutes). Identity's clusters at that load are too big to be usable at all.
- **Where each hash turns:** Fibonacci is best at 2^24 and gets *worse* at 2^25 (129 → 150 ns). Identity is still improving at 2^25 (107 → 90 ns).
  - My explanation, which I haven't tested, is locality. Identity keeps recently issued refs next to each other, so neighbouring lookups share cache lines and memory pages. Fibonacci's random scatter over 768 MB touches a new page almost every time.

## Per-message latency (store + books)

**Method:**
- `scale_bench <file> latency1 <variant> <outdir> 2`: every book message timed on its own with the same method as M8 (fenced TSC, log-linear histogram, thread pinned to CPU 2).
- 2 runs per variant, 526M samples merged.
- The **40 ns timer cost is included**.
- **Every run produced the full-day state hash `5cdfcc85f1d6976a`**, so all four variants built the identical book.

| ns per book message (2 runs merged) | p50 | p90 | p99 | p99.9 | p99.99 | mean |
|---|---|---|---|---|---|---|
| identity 2^23 | **176** | 448 | 1,090 | **6,155** | 29,752 | 246 |
| fibonacci 2^23 | 248 | 496 | 1,090 | **2,821** | 23,596 | 298 |
| identity 2^24 | **168** | **416** | **897** | 2,436 | 19,492 | **216** |
| fibonacci 2^24 | 248 | 496 | 1,090 | 2,821 | 15,389 | 296 |

- **Run-to-run spread:**
  - p50 to p99.9 agree within ~10% between each variant's two runs.
  - p99.99 does not. Fibonacci 2^24 gave 11,798 and 22,570, and identity 2^23 gave 29,752 and 28,726. So p99.99 differences under ~2× aren't reliable.
- **The max was 4–81 ms in every run.** That's Windows preemption, not the table, and it isn't compared here.

**At 2^23 the trade-off is plain.** Identity's median is 29% lower, but its p99.9 is **2.2× worse** (6,155 vs 2,821 ns). That's the tail its long probe runs produce: a worst case of 13,253 slots against 11.

**At 2^24 identity wins or ties at every percentile we can trust** (p50 −32%, p90 −16%, p99 −18%, p99.9 −14%). Its p99.99 is within the run-to-run noise of Fibonacci's.

## The decision: identity, 2^24 slots, growing at 25% load

> **Identity hashing is ~32% faster than Fibonacci at the median and better through p99.9 at 2^24 slots (168 vs 248 ns p50, 2,436 vs 2,821 ns p99.9, full day, store + books). It degrades badly with load, though. At 23% full its p99.9 is already 2.2× worse than Fibonacci's; at 46% it's 3.6× slower on average; at 92% it didn't finish. So it ships with a guard: the table doubles once it passes 25% full, and it starts at 2^24 slots, 8.7× the observed full-day peak of 1,924,078 live orders. If the order count were unpredictable, Fibonacci would be the safer default: slower at every size we can run, but its worst probe stays bounded at any load (31 slots at 46% full, 890 at 92%).**

**The pieces:**
- **The measured choice:** identity at 2^24. Better or equal at p50 through p99.9 (p50 −32%, p90 −16%, p99 −18%, p99.9 −14%), and 17% better store-only throughput (107 vs 129 ns/msg).
- **The named risk:** identity's failure is a cliff, not a slope. The tail goes first (23% full), then the average (46%), then everything (92%). Fibonacci at the same loads slows gradually: 140, 170 and 340 ns/msg.
- **The mitigation:** a 25% growth threshold (`MaxLoadPercent = 25`) keeps identity on the good side of the cliff. Starting at 2^24 means the full day never reaches it (peak load 11%). A day with 4.2M live orders would trigger one doubling to 2^25, where identity measured fastest of all (90 ns/msg). That costs one rehash and 768 MB, rather than a slide down the cliff.
- **What it costs:** 384 MB for the table from start-up, against 48 MB before.
- **What would change the decision:** unbounded or unpredictable order counts, or no memory to spare. Then Fibonacci at 2^24 is the choice: ~22 ns/msg slower in store cost, and its worst case stays bounded.

**Two corrections to the brief this came from:**
- The headroom is **8.7×**: 16,777,216 slots against the full-day peak of 1,924,078. 9.5× was against the 4 GB partial file's peak of 1.77M.
- The degradation doesn't start "above ~50%". The average collapsed at **46%**, and the tail was already 2.2× worse at **23%**. That's why the guard is 25%, not 50%.

**Implemented** as the default in `src/core/order_store.h`:
```cpp
FlatStore<AoS, SentinelKey, Identity, BackwardShift, 1 << 24, /*Grow=*/true, /*MaxLoadPercent=*/25>
```
**Checked:**
- All 6 test suites pass. `store_tests` now also runs the 25%-guard design against `std::unordered_map`, and checks that a 64-slot table holds 16 entries and doubles on the 17th.
- On the sample, 2^24 costs the same as 2^22 (39–44 ns/msg store-only, 3 interleaved runs each), so the bigger table doesn't slow small days down.
