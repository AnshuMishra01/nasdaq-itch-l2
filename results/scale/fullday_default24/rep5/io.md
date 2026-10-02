# scale_bench io

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 1 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 13.1 s | 13.1 s (630 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 17.8 s | 13.2 s (626 MB/s) | 4.6 s | 17.5 | 67.7 |
| store only (default store), rep 1 | 32.2 s | 12.9 s (639 MB/s) | 19.3 s | 73.2 | 122.4 |
| store + books (default), rep 1 | 66.2 s | 12.8 s (643 MB/s) | 53.4 s | 202.7 | 251.6 |

peak private memory 476 MB, peak working set 479 MB
