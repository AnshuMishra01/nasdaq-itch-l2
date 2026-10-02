# scale_bench latency1 fibonacci23fixed

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## Per-message latency: fibonacci23fixed

Book messages (A F E C X D U), decode + order store + books, fenced TSC (1.9962 ticks/ns), timer cost 40.1 ns (included below; subtract for the work alone), pinned to CPU 2. Values are histogram bucket upper bounds (<= 6.25% high).

| run | messages | p50 | p90 | p99 | p99.9 | p99.99 | max | mean | wall ns/msg (probed) | state hash |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 263241937 | 232 | 464 | 993 | 2821 | 21544 | 12924336 | 282.0 | 337.2 | 5cdfcc85f1d6976a |
| 2 | 263241937 | 248 | 545 | 1154 | 2949 | 25648 | 81158437 | 313.6 | 369.9 | 5cdfcc85f1d6976a |
| **merged** | 526483874 | 248 | 496 | 1090 | 2821 | 23596 | 81158437 | 297.8 | | |

peak private memory 292 MB, peak working set 294 MB
