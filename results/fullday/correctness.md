# Full-day correctness: 30 December 2019

**File:** `data/12302019.NASDAQ_ITCH50.itch50`, 8,251,407,909 bytes, decompressed from Nasdaq's `12302019.NASDAQ_ITCH50.gz`, which passed `gzip -t`. It holds **268,744,780 messages** from 03:04:32 to the end-of-day event at 20:05:00, of which **263,241,937 are book messages** (A, F, E, C, X, D, U).

**Run:** `itch_count <file> 0 results/fullday/verify.txt verify` (chunked reading). Raw output is in `verify.txt`.

## 1. Every message against the spec

- **All 268,744,780 messages** were framed, and each one's length matched the size the ITCH 5.0 spec gives for its type.
- **No unknown types**, and the file ends exactly on a message boundary: no truncation warning.
- **Message types seen**, with counts (`message_types.txt`): A 117.1M · D 114.4M · U 21.6M · E 5.7M · I 4.0M · X 2.8M · F 1.5M · P 1.2M · L 215k · C 99.9k · Q 17.8k · Y 9.0k · H 9.0k · R 8.9k · K 3 · S 6 · J 34 · V 1.
  - **C, Q, I and J ran against real bytes for the first time.**
  - **B, N, W and h do not occur** in this file, so their sizes are still checked only against the spec, not against real data.
- **Decoded field offsets** were exercised for every type the book uses (A F E C X D U P Q). The decoder unit tests cover all of them byte by byte, and the closing-cross prices below check C and Q end to end.

## 2. Anomaly counters: all zero (see also section 6: no crossed book while trading)

| counter | full day |
|---|---|
| execute / cancel / delete / replace of an unknown order | 0 / 0 / 0 / 0 |
| add of an order that already exists | 0 |
| over-execution / over-cancel | 0 / 0 |
| price level missing / level going below zero | 0 / 0 |

Peak live orders: **1,924,078**.

## 3. Store vs book cross-check: PASS at every checkpoint

Every 500,000 messages, the price levels were rebuilt from the order store and compared with the books (shares, order count, sort order). All **528 checkpoints passed**, plus the end of file: 0 mismatches of any kind. At the busiest checkpoints that meant ~1.79M orders against ~754k price levels.

**At the end of the file, both the store and the books are empty: 0 orders and 0 levels.** Every order created during the day (118.6M by A and F, plus 21.6M as the new side of a U) was eventually removed by an execution, cancel, delete or replace. Nothing leaked, nothing was left behind, and nothing was removed twice. (The cross-check only proves the two structures agree; ending at zero is a separate check that the bookkeeping balances over the whole day.)

## 4. State hash

**Full day: `5cdfcc85f1d6976a`.**
- The `verify` run produced it with the default store at the time (identity, 2^21 growing at 50%).
- All 8 latency runs (identity and Fibonacci at 2^23 and 2^24, `results/scale/fullday/latency1_*.md`) produced the same hash, including identity at 2^24, the new default.
- The order-store part of the hash doesn't depend on table layout, so equal hashes mean identical books.

A fresh `verify` run with the new default was started but stopped by Claude Code when the system ran low on memory. It hasn't been re-run.

## 5. Against the outside world: closing prices from a public source

**What's compared.** The program's closing price for a stock is the price in its **closing cross** `Q` message (cross type `C`): Nasdaq's 16:00 closing auction. For Nasdaq-listed stocks this is the official close (Nasdaq Official Closing Price), and it's what most end-of-day sources publish as "close". It is **not** the last trade of the day: AAPL's last trade was at 19:59:16, after hours, at 291.60, while its close was 291.52.

**Source.** Yahoo Finance daily chart data for 30 Dec 2019 (`query1.finance.yahoo.com/v8/finance/chart/<SYM>`). Its `close` field is adjusted for later stock splits but not for dividends (dividends go in a separate `adjclose` field). The split history from the same source shows AAPL split 4:1 on 31 Aug 2020 and AMZN 20:1 on 6 Jun 2022, and no splits for MSFT, INTC or CSCO since. Raw responses, fetched 2 Oct 2026, are in `external/`.

| | ours: closing cross | Yahoo `close` | split since | Yahoo × split | |
|---|---|---|---|---|---|
| AAPL | 291.52 | 72.88 | 4:1 | 291.52 | ✅ exact |
| MSFT | 157.59 | 157.59 | — | 157.59 | ✅ exact |
| AMZN | 1,846.89 | 92.3445 | 20:1 | 1,846.89 | ✅ exact |
| INTC | 59.62 | 59.62 | — | 59.62 | ✅ exact |
| CSCO | 47.59 | 47.59 | — | 47.59 | ✅ exact |

**All five match to the cent.** This is the only check in the project that tests the program against something it didn't produce itself. It means:
- the decoding is right, including the 8-byte share count and 4-byte price inside `Q`;
- the price scaling is right (4 implied decimal places);
- the program picks the right message for "close".

**The opens, compared the same way:**
- **They match for MSFT (158.99), AMZN (1,874.00), INTC (59.99) and CSCO (47.75).**
- **AAPL does not:** our opening cross is 289.44, while Yahoo's open × 4 = 289.46, 2 cents apart. Not explained yet. A plausible cause is a different definition of "open" (for example, the first consolidated trade rather than Nasdaq's opening cross), but I haven't checked it.

**A note on how this check started:** the AAPL figure was first compared with a number given to me in conversation, which isn't a source. It's now checked against published data, as above.

## 6. Crossed books: none while trading

`itch_count <file> 0 results/fullday/book_trading_state.txt book`. The handler tracks each stock's trading state from its `H` messages, and the settling period after each cross.

| crossed updates (best bid ≥ best ask after an add) | count |
|---|---|
| any state | 4,777 |
| stock halted, paused or quotation-only (allowed: no matching) | 4,767 |
| within 100 ms of a cross that left the book crossed (allowed: settling) | 10 |
| **stock trading and not settling** | **0** |

**The 10 settling cases are all halt reopening crosses:** MKD at 10:54:21.569, 11:21:52.139 and 11:47:19.720, and MBOT at 14:20:41.222. Message dumps show the same sequence each time:
1. the reopening cross executions (`C`, non-printable) and the `Q` (cross type H);
2. at that same nanosecond, `H` switches the stock to T;
3. then **another 0.2–0.5 ms of non-printable `C` executions**, and, for MBOT, the deletion of a partially filled order's unfilled remainder.

Orders added during that window see a book that still holds matched orders.

**The rule:**
- Settling starts at a `Q` only if the book is crossed at that moment.
- It ends at the first update that leaves the book uncrossed.
- It excuses crossings only within **100 ms** of the cross.

Of 26 settles, the ones over 1 ms were after-close or halted stocks (1–12 ms, plus PNRL, halted all day). MKD's opening cross left its book crossed until 10:45, but MKD wasn't in state T during that time: with the 100 ms cap in place the counts are unchanged, so no crossing while trading was excused by a long settle.

## 7. The opening auction

See `auction.md`. At the 09:30 cross, the book orders that trade are removed by `C` messages with printable = `N`, stamped at exactly the same nanosecond as that stock's `Q`. They're not removed by D or E. The handler already treats them correctly.
