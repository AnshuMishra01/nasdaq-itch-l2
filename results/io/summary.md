# Input modes on a 4 GB file: load vs mmap vs chunked

**Question.** The program used to read the whole file into one `std::vector`. That was fine at 96 MB, but the full day decompresses to ~8–9 GB on a 16 GB laptop. Should it memory-map the file, or read it in chunks?

**Setup:**
- **File:** `data/partial_day.itch50`, 4,000,099,237 bytes, the first 4 GB of the 30 Dec 2019 file decompressed. It covers 03:04 to past 09:30 and holds **128,645,785 book messages**. It ends 37 bytes into a message, because the download was cut off; that tail is reported, and everything before it is processed.
- **Run:** `itch_count <file> 0 <out> book --io=<mode>`, with the timer around the whole parse.
- **Rounds:** 3, alternating which mode runs first.
- **Machine:** Ryzen 5 7530U laptop, Windows 11, g++ 16.2 `-O3`, statically linked.
- **Conditions:** about 3 GB of RAM free, and a background download writing to the same disk at ~100 KB/s.
- **Raw output:** `book_<mode>_r<n>.txt` and `verify_<mode>.txt` in this folder.

## Correctness first

`verify` mode on the 4 GB file, once per mode:

| | mmap | chunked |
|---|---|---|
| state hash | `a9424b9bfc7430ae` | `a9424b9bfc7430ae` |
| store-vs-book cross-check | PASS at every checkpoint | PASS at every checkpoint |
| order-store anomalies, level missing/underflow | 0 | 0 |
| truncated tail | 37 bytes, reported | 37 bytes, reported |

Identical books. On the sample, all three modes (load, mmap, chunked) produce the golden hash `96fd73902096c354`, checked by `ctest`. `tests/io_tests.cpp` also parses the same bytes in chunks of 1, 7, 37, 4,096 and 1 MB and requires the same messages in the same order. When I planted a bug that drops the partial message at a chunk boundary, 14 of its checks failed.

## Results

| mode | round 1 | round 2 | round 3 | peak working set | peak private memory | page faults |
|---|---|---|---|---|---|---|
| mmap | 131.4 s | 102.7 s | 101.8 s | 3,941 MB | 195 MB | 1,029,358 |
| chunked (64 MB) | 125.2 s | 105.6 s | 100.5 s | 255 MB | 252 MB | 67,308 |
| load | — | — | — | — | — | — |

- **Load didn't finish.** Claude Code stopped the job because the system ran critically low on memory: a 4 GB vector with ~3 GB free. That's the problem this experiment set out to fix, happening for real. It wasn't re-run.
- **Measures:**
  - **Working set** = RAM the process is using, including file pages mapped in by mmap.
  - **Private** = memory the process itself owns (heap, vectors, the 64 MB chunk buffer).
  - **Page faults** = soft + hard, over the whole process (Windows `PageFaultCount`).
- **Round 1 was slower for both modes** (cold file cache), so compare rounds 2 and 3.

**Speed: a tie.** Rounds 2 and 3 are 102.7 / 101.8 s for mmap and 105.6 / 100.5 s for chunked, within 3% of each other and inside the run-to-run spread. Reading the file isn't the bottleneck.

**Memory: chunked wins clearly.**
- **mmap's working set grows to the size of the file (3.9 GB)**, because every page it touches stays mapped until Windows decides to trim it. Those pages are clean and could be dropped, but they still crowd a machine with 3 GB free. At 8–9 GB for a full day, that's more than the free RAM.
- **Chunked stays at ~255 MB** whatever the file size: the 64 MB buffer, plus the order store, books and so on.

**Page faults: chunked has 15× fewer.**
- mmap takes about **1 million page faults**, one per 4 KB page of the file, **inside the timed parse loop.** Every one of those is a potential latency spike on whichever message happens to touch a new page.
- Chunked reads between chunks, at points we choose, so its faults are only the program's own memory.
- The effect on the *per-message* latency tail wasn't measured: `latency_bench` still loads the whole sample file.

## Decision

**Chunked is now the default** (`--io=chunked`). It's as fast as mmap here, uses a fixed ~255 MB instead of the file's size in RAM, and keeps disk reads out of the message loop. mmap and load stay available through `--io=mmap` and `--io=load`.

## A bigger finding from the same runs: the full day is ~11× slower per message

| | sample (pre-market) | 4 GB partial day |
|---|---|---|
| book messages | 3,197,545 | 128,645,785 |
| peak live orders | 27,221 | **1,769,537** |
| price levels at the end | 23,162 | **749,587** |
| ns per book message | ~67–69 | **~780–820** (rounds 2–3, both modes) |

**This isn't the I/O:** mmap and chunked are equally slow, and the parse floor is ~20 ns. Something in the order store or the books costs about 10× more at full-day scale. Candidates, **none measured yet**:
- **The order store at higher load.** 1.77M live orders is past the point where the 2M-slot table grows (load 0.5). After doubling to 4M slots it runs at ~0.42 load, where M9 measured identity hashing's worst probe at 210 slots, against 12 for Fibonacci. This is the open question M9 flagged, and the first suspect.
- **Much deeper books.** 750k levels across the market means some sides are far deeper than the ~9 levels M5 measured, so inserting a price far from the best shifts many elements.
- **Cache pressure.** The working set is now ~200+ MB of store and books rather than a few MB.

**Next step:** re-run the M9 store experiment and the M8 latency split on this file, to find which one it is before changing anything.
