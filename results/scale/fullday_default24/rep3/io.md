# scale_bench io

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 1 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 11.9 s | 11.9 s (691 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 17.1 s | 12.5 s (661 MB/s) | 4.6 s | 17.4 | 65.0 |
| store only (default store), rep 1 | 32.4 s | 12.2 s (674 MB/s) | 20.1 s | 76.3 | 123.0 |
| store + books (default), rep 1 | 69.7 s | 12.7 s (648 MB/s) | 56.9 s | 216.3 | 264.8 |

peak private memory 476 MB, peak working set 479 MB
