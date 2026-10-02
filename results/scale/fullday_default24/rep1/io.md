# scale_bench io

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 1 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 14.2 s | 14.2 s (581 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 21.4 s | 16.2 s (510 MB/s) | 5.2 s | 19.9 | 81.4 |
| store only (default store), rep 1 | 43.8 s | 15.7 s (527 MB/s) | 28.1 s | 106.8 | 166.5 |
| store + books (default), rep 1 | 74.9 s | 14.9 s (552 MB/s) | 59.9 s | 227.6 | 284.5 |

peak private memory 476 MB, peak working set 479 MB
