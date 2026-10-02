# scale_bench io

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 1 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 13.9 s | 13.9 s (595 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 19.2 s | 14.2 s (583 MB/s) | 5.0 s | 18.8 | 72.8 |
| store only (default store), rep 1 | 36.7 s | 15.6 s (529 MB/s) | 21.1 s | 80.1 | 139.5 |
| store + books (default), rep 1 | 74.7 s | 15.4 s (537 MB/s) | 59.3 s | 225.2 | 283.8 |

peak private memory 476 MB, peak working set 479 MB
