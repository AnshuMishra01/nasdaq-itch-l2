# scale_bench io

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 1 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 13.5 s | 13.5 s (610 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 17.1 s | 12.7 s (651 MB/s) | 4.4 s | 16.8 | 65.0 |
| store only (default store), rep 1 | 33.7 s | 12.9 s (642 MB/s) | 20.8 s | 79.1 | 128.1 |
| store + books (default), rep 1 | 70.6 s | 13.3 s (622 MB/s) | 57.3 s | 217.5 | 268.1 |

peak private memory 476 MB, peak working set 479 MB
