# M9 decision: which order store, and why

Written 2026-09-27 from the files in this folder (`summary.md` plus one `.txt` per variant).
Reproduce with: `./build/store_bench.exe data/12302019.NASDAQ_ITCH50.itch50.sample 3 results/m9`
(for new data or new variants, pass a different output folder so this record stays intact).

## Decision

```cpp
using Store = FlatStore<Layout::AoS, EmptyMark::SentinelKey, HashFn::Identity, Erase::BackwardShift>; // 2^21 slots
```

- **Store cost:** 70 → 15 ns/msg (−79%). The target was under 20.
- **`itch_count` book mode:** ~128 → ~75–83 ns/msg.
- **Correctness:** same state hash as `unordered_map` (`96fd73902096c354`), 0 anomalies, and the store-vs-book cross-check passes.

## How it was measured

- **Machine and data:** Ryzen 5 7530U laptop, g++ 16.2 `-O3 -DNDEBUG`, whole sample file (3.2M book messages, peak 27,221 live orders).
- **Two timings per variant:** *store + books* (the real workload) and *store only* (books switched off at compile time). **Store cost = store only − floor**, where the floor is parse + decode + discard, measured in the same session.
- **3 runs per variant, interleaved** (run 1 of every variant, then run 2, ...) so laptop drift hits them all equally. Medians are reported.
- **The whole experiment ran 3 times.** The conclusions below hold in all three. Absolute numbers drift between sessions (the floor was 22 ns this time, 17 in earlier sessions), so compare variants **within one session only**.

Store cost in ns/msg across the three sessions:

| variant | session 1 | session 2 | session 3 (the files here) |
|---|---|---|---|
| 00 unordered_map | 66.2 | 68.0 | 70.4 |
| 01 flat base | 13.7 | 14.5 | 14.8 |
| 02 Fibonacci hash | 39.8 | 41.4 | 44.5 |
| 03 SoA layout | 13.9 | 12.6 | 15.3 |
| 04 metadata byte | 15.0 | 15.1 | 17.2 |
| 05 tombstones | 18.7 | 20.0 | 19.2 |
| 06 grow from 1,024 | 25.5 | 26.9 | 27.7 |
| 07 2^23 slots | 15.9 | 15.3 | 14.3 |
| 08 direct index | 8.6 | 8.6 | 9.9 |
| 09 2^16 fixed | — | 14.0 | 16.2 |
| 10 2^18 fixed | — | 9.0 | 10.2 |
| 11 Fibonacci, 2^16 | — | 12.9 | 14.2 |

Anything within about 2 ns of each other swaps order between sessions: that's noise, not a result.

## The design questions, answered by the data

**1. Layout: AoS or SoA? → AoS (a tie).**
SoA was 13.9 / 12.6 / 15.3 against AoS 13.7 / 14.5 / 14.8, so neither is consistently faster. The expected SoA advantage (8 keys per cache line while probing) never shows because probes are almost never needed: mean probe distance is **0.00, max 1**. Every lookup lands on its home slot, so what counts is one cache miss per hit, and AoS gets key and value in that one miss. AoS is simpler and costs 8 MB more (48 vs 40 MB). Revisit if longer probe runs ever appear.

**2. Empty marker: key 0 or a metadata byte? → key 0.**
The metadata byte was 1–2 ns slower in every session (an extra array to touch) and uses 2 MB more. Order refs are never 0, and the store refuses refs 0 and ~0 and counts them (`rejected refs`, 0 in this data), so a bad feed can't silently corrupt it.

**3. Hash: identity or Fibonacci? → identity, for this data. This is the main finding.**
- **The surprise:** Fibonacci costs about 3× as much (40–45 vs 14–15 ns), yet probe distances are almost identical (0.01 vs 0.00). So it isn't collisions. It's **where the entries land in memory.**
- **Why identity wins:** order refs are handed out in increasing order, and live orders are mostly recent ones. With `ref & mask`, recent orders sit in neighbouring slots, a small region of a 48 MB table that stays in cache. Fibonacci deliberately scatters them across all 48 MB, so nearly every lookup misses cache.
- **The proof:** shrink the table to 2^16 (1.5 MB, fits in cache) and the gap disappears. Fibonacci 12.9 / 14.2 vs identity 14.0 / 16.2 (variants 11 vs 09).
- **The catch:** identity hashing clusters badly once the table fills up. At 2^16 (load 0.41) the worst probe distance is **210** with identity and **12** with Fibonacci. Identity is only fast while the table is mostly empty.

**4. Capacity: size up front or grow? → large up front (2^21), with growth kept as a safety net.**
- **Growing from 1,024 slots cost 26–28 ns.** The growing table ends with exactly the same contents as a fixed 2^16 table (variants 06 vs 09, same probe distances), and its 6 rehashes of at most 32k entries cost well under a millisecond in total. So the extra cost isn't the rehashing itself.
- **The likely cause is load.** While growing, the table spends most of the file between load 0.25 and 0.5. With identity hashing that means long runs. The fixed 2^16 table sits near load 0.1 for most of the file. (This is inferred from the numbers; load over time wasn't measured.)
- **Lesson:** with identity hashing, keep the load low. Sizing is part of the hash decision.
- **2^18 (6 MB) was fastest of all, at 9–10 ns:** it fits in the L3 cache and still has load 0.1. Not chosen, because a full trading day will have far more than 27k live orders. 2^21 leaves room: load stays under 0.25 up to about 500k live orders.

**5. Delete: tombstones or backward shift? → backward shift.**
Tombstones were 4–5 ns slower in every session and needed a rebuild (1 rehash) as deleted slots piled up. Almost every order is eventually deleted (peak live is only 27k in a file of 3.2M book messages), so tombstones build up quickly. Backward shift keeps every run tight and needs no rebuilds.

**Direct indexing (08) was fastest (8.6–9.9) but isn't used.** Its memory grows with the *largest ref ever seen* (104 MB for this pre-market sample), not with live orders. Over a full day that becomes hundreds of MB to GB. It's here only as a lower bound.

## Correctness of every variant

All 12 variants: state hash `96fd73902096c354` (same as `unordered_map`), same store hash with and without books, 0 anomalies, `crossed_updates` 2 (the halted PNRL, as before), and the store-vs-book cross-check passes.

`tests/store_tests.cpp` runs every variant against `std::unordered_map` over 200k random add/find/remove operations, in tables that start at 16 slots.
- **A random test alone missed a real bug.** A deliberately planted wrap-around bug in backward shift got past it.
- **A hand-built test now catches it:** a run crossing the end of the table.

## Open questions: re-run on the full-day file

1. **Peak live orders over a full day are unknown.** Identity hashing is only safe at low load. If the peak goes past ~500k (load 0.25 at 2^21), re-run this bench with `results/m9-fullday` and compare against Fibonacci and a lower growth threshold.
2. **Would 2^18 plus a lower growth threshold (e.g. 0.25) keep its 9–10 ns on a full day?** Not measured.
3. **Consider a hash that keeps nearby refs nearby but spreads clusters**, e.g. `(ref ^ (ref >> k)) & mask`. Not tried.
