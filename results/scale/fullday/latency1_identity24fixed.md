# scale_bench latency1 identity24fixed

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## Per-message latency: identity24fixed

Book messages (A F E C X D U), decode + order store + books, fenced TSC (1.9962 ticks/ns), timer cost 40.1 ns (included below; subtract for the work alone), pinned to CPU 2. Values are histogram bucket upper bounds (<= 6.25% high).

| run | messages | p50 | p90 | p99 | p99.9 | p99.99 | max | mean | wall ns/msg (probed) | state hash |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 263241937 | 168 | 416 | 897 | 2564 | 17441 | 5160778 | 216.0 | 271.4 | 5cdfcc85f1d6976a |
| 2 | 263241937 | 168 | 416 | 897 | 2436 | 20518 | 13666642 | 216.0 | 269.2 | 5cdfcc85f1d6976a |
| **merged** | 526483874 | 168 | 416 | 897 | 2436 | 19493 | 13666642 | 216.0 | | |

peak private memory 484 MB, peak working set 486 MB
