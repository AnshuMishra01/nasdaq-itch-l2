# Predictions, written before the full-scale store sweep and book profile were run

Written 2026-09-29, after experiment 1 (`io.md`) but before any other full-scale store or book numbers existed. Kept unedited so the results can be checked against them.

**Known going in (from `io.md`):** the order store costs ~555 ns/msg at full scale, against ~15 pre-market. Books add ~180 ns. I/O is ~45 ns/msg.

1. **The current default (identity, starts at 2^21, doubles at load 0.5)** will show a load of about 0.42 at the end (1.77M live in 4M slots). Its maximum probe distance will be in the hundreds or more, and its mean well above 1, because identity hashing clusters at that load (M9 saw a worst case of 210 at 0.41).
2. **Bigger identity tables (2^23, 2^24) will be much faster** than the default: lower load means shorter runs. The cost will flatten somewhere around 2^23.
3. **Fibonacci will beat identity at the default size**, because it doesn't cluster. But at 2^23 and 2^24 identity will win again, because Fibonacci scatters live orders across a 200–400 MB table, and cache misses dominate.
4. **Even the best variant will stay well above pre-market's 15 ns.** 1.77M live orders × 24 bytes is ~42 MB, far past the 16 MB L3 cache, so most lookups will miss cache whatever the hash. My guess is 60–150 ns at best.
5. **Books after 09:30** will have a p50 depth well above pre-market's, and updates less concentrated near the top. Rank < 3 will fall below the pre-market 83%. More than 10% of updated sides will be deeper than 16 levels, so reserve(16) will reallocate more often.
