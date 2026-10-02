# scale_bench latency1 fibonacci24fixed

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## Per-message latency: fibonacci24fixed

Book messages (A F E C X D U), decode + order store + books, fenced TSC (1.9962 ticks/ns), timer cost 40.1 ns (included below; subtract for the work alone), pinned to CPU 2. Values are histogram bucket upper bounds (<= 6.25% high).

| run | messages | p50 | p90 | p99 | p99.9 | p99.99 | max | mean | wall ns/msg (probed) | state hash |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 263241937 | 248 | 545 | 1154 | 2693 | 11798 | 4471466 | 305.9 | 361.0 | 5cdfcc85f1d6976a |
| 2 | 263241937 | 232 | 464 | 1090 | 2949 | 22570 | 9625749 | 285.4 | 340.5 | 5cdfcc85f1d6976a |
| **merged** | 526483874 | 248 | 496 | 1090 | 2821 | 15389 | 9625749 | 295.6 | | |

peak private memory 484 MB, peak working set 486 MB
