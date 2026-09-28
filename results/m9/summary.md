# M9 order-store experiment

- date: 2026-09-27 14:52:34
- file: `data/12302019.NASDAQ_ITCH50.itch50.sample`, 3 interleaved reps per variant, medians shown
- compiler: g++ 16.2.0, optimised, NDEBUG
- floor (parse + decode + discard): 22.4 ns/msg
- store cost = store-only median - floor
- correct = state hash equals the golden hash 96fd73902096c354, store hash equal with and without books, 0 anomalies, same crossed count as baseline, cross-check PASS

| variant | change | store+books ns/msg | store-only ns/msg | store cost | vs baseline cost | store | correct |
|---|---|---|---|---|---|---|---|
| 00_unordered_map | baseline | 165.5 | 92.8 | 70.4 | +0% | memory n/a (node-based) | yes |
| 01_flat_base | vs 00: node-based map -> flat open addressing | 81.6 | 37.2 | 14.8 | -79% | capacity 2097152 slots, 48.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.00 max 1 | yes |
| 02_hash_fibonacci | vs 01: hash identity -> fibonacci | 110.8 | 66.9 | 44.5 | -37% | capacity 2097152 slots, 48.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.01 max 1 | yes |
| 03_layout_soa | vs 01: layout AoS -> SoA | 80.9 | 37.7 | 15.3 | -78% | capacity 2097152 slots, 40.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.00 max 1 | yes |
| 04_empty_metabyte | vs 01: empty marker sentinel key -> metadata byte | 82.8 | 39.6 | 17.2 | -76% | capacity 2097152 slots, 50.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.00 max 1 | yes |
| 05_erase_tombstone | vs 01: delete backward-shift -> tombstones | 84.2 | 41.6 | 19.2 | -73% | capacity 2097152 slots, 48.0 MB, rehashes 1, rejected refs 0, probe distance mean 0.00 max 1 | yes |
| 06_capacity_grow | vs 01: capacity 2^21 fixed -> 2^10 + growth | 94.7 | 50.1 | 27.7 | -61% | capacity 65536 slots, 1.5 MB, rehashes 6, rejected refs 0, probe distance mean 0.61 max 210 | yes |
| 07_capacity_2^23 | vs 01: capacity 2^21 -> 2^23 | 81.5 | 36.7 | 14.3 | -80% | capacity 8388608 slots, 192.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.00 max 0 | yes |
| 08_direct_index | reference point: no hashing at all | 71.7 | 32.3 | 9.9 | -86% | 104.0 MB | yes |
| 09_capacity_2^16 | vs 01: capacity 2^21 -> 2^16; vs 06: growth removed | 83.4 | 38.6 | 16.2 | -77% | capacity 65536 slots, 1.5 MB, rehashes 0, rejected refs 0, probe distance mean 0.61 max 210 | yes |
| 10_capacity_2^18 | vs 01: capacity 2^21 -> 2^18 | 84.1 | 32.6 | 10.2 | -86% | capacity 262144 slots, 6.0 MB, rehashes 0, rejected refs 0, probe distance mean 0.05 max 4 | yes |
| 11_fibonacci_2^16 | vs 09: hash identity -> fibonacci | 78.9 | 36.6 | 14.2 | -80% | capacity 65536 slots, 1.5 MB, rehashes 0, rejected refs 0, probe distance mean 0.37 max 12 | yes |
