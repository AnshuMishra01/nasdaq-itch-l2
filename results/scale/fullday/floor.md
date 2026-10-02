# scale_bench floor

file: `data/12302019.NASDAQ_ITCH50.itch50` (8251407909 bytes)

| run | wall | in read() | parsing | parse ns/msg | wall ns/msg |
|---|---|---|---|---|---|
| floor (parse + decode, discard) | 20.9 s | 14.6 s (565 MB/s) | 6.2 s | 23.7 | 79.3 |

peak private memory 65 MB, peak working set 69 MB
