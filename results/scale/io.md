# scale_bench io

file: `data/partial_day.itch50` (4000099237 bytes)

## I/O vs processing

Each run reads the file in 64 MB chunks. `in read()` is time blocked in `ifstream::read`; `parsing` is framing + decode + handler. Interleaved, 3 reps.

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| read only, rep 1 | 6.0 s | 6.0 s (671 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 1 | 9.1 s | 6.1 s (655 MB/s) | 3.0 s | 23.3 | 70.9 |
| store only (default store), rep 1 | 82.3 s | 5.8 s (692 MB/s) | 76.5 s | 594.5 | 639.8 |
| store + books (default), rep 1 | 104.7 s | 5.8 s (684 MB/s) | 98.8 s | 767.9 | 813.6 |
| read only, rep 2 | 6.9 s | 6.9 s (584 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 2 | 9.0 s | 6.0 s (662 MB/s) | 3.0 s | 23.0 | 70.1 |
| store only (default store), rep 2 | 79.9 s | 5.6 s (716 MB/s) | 74.2 s | 577.0 | 620.7 |
| store + books (default), rep 2 | 103.2 s | 5.6 s (712 MB/s) | 97.6 s | 758.4 | 802.5 |
| read only, rep 3 | 5.8 s | 5.8 s (686 MB/s) | - | - | - |
| floor (parse + decode, discard), rep 3 | 8.8 s | 5.9 s (682 MB/s) | 2.9 s | 22.7 | 68.5 |
| store only (default store), rep 3 | 80.3 s | 5.5 s (725 MB/s) | 74.8 s | 581.2 | 624.3 |
| store + books (default), rep 3 | 102.9 s | 5.4 s (735 MB/s) | 97.4 s | 757.2 | 799.8 |

peak private memory 252 MB, peak working set 256 MB
