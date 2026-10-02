# scale_bench latency1 identity23fixed

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## Per-message latency: identity23fixed

Book messages (A F E C X D U), decode + order store + books, fenced TSC (1.9962 ticks/ns), timer cost 40.1 ns (included below; subtract for the work alone), pinned to CPU 2. Values are histogram bucket upper bounds (<= 6.25% high).

| run | messages | p50 | p90 | p99 | p99.9 | p99.99 | max | mean | wall ns/msg (probed) | state hash |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 263241937 | 176 | 464 | 1154 | 6155 | 29752 | 16338173 | 251.0 | 306.7 | 5cdfcc85f1d6976a |
| 2 | 263241937 | 176 | 432 | 1090 | 6155 | 28726 | 14281455 | 241.2 | 296.5 | 5cdfcc85f1d6976a |
| **merged** | 526483874 | 176 | 448 | 1090 | 6155 | 29752 | 16338173 | 246.1 | | |

peak private memory 292 MB, peak working set 294 MB
