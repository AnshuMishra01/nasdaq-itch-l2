# What removes matched orders at the 09:30 opening cross?

**Question.** At the opening auction, Nasdaq matches buy and sell orders and publishes one `Q` (cross trade) per stock. The `Q` names no orders. So which messages take the matched orders out of the book? The two candidates were a burst of `D` (delete), or `E` (executed).

**Data:** the full day, 30 Dec 2019 (`data/12302019.NASDAQ_ITCH50.itch50`, 268,744,780 messages).
**Tools and output:**
- `itch_window` (counts per type per time bucket): `auction_1s.txt`, `auction_10ms_a.txt`
- `open_cross` (same-stock matching): `open_cross.txt`

## 1. Counting per second is misleading

| type | 09:29:59 | 09:30:00 |
|---|---|---|
| D delete | 4,290 | 69,767 |
| E executed | 187 | 2,497 |
| C executed with price | 0 | 886 |
| A + F adds | 5,315 | 383,047 |
| Q cross | 0 | 8,657 |

**Everything** explodes at the open, adds most of all, because traders pull and replace orders in bulk. D is the largest removal type by count, but a per-second count can't say whether those deletes have anything to do with the cross.

At 10 ms resolution, the `Q` messages trickle out at 50–100 per 10 ms from 09:30:00.001 to 09:30:01.019, one stock at a time. E and C stay at about 5–30 per 10 ms and don't follow them. There are fewer executions (2,497 E + 886 C) than crosses (8,657).

## 2. Matching each cross to the same stock's messages

For each of the **3,086 opening crosses that traded shares** (19,896,150 shares in total; the other 5,820 stocks crossed with zero volume), `open_cross` looks at that stock's E, C and D messages from 09:29:58 to 09:30:03, sorted by their time relative to that stock's `Q`:

| offset from the stock's Q | E | C, printable = N | C, printable = Y | D |
|---|---|---|---|---|
| before | 375 msgs / 69,414 sh | 0 | 2 / 106 sh | 6,004 |
| **same nanosecond** | **0** | **860 msgs / 344,232 sh** | **0** | **0** |
| after | 4,130 msgs / 623,913 sh | 0 | 175 / 106,360 sh | 46,649 |

## Answer

- **Neither D nor E.** The book orders matched in the opening cross are removed by **`C` (order executed with price) messages with printable = `N`**, time-stamped **at exactly the same nanosecond as that stock's `Q`**. They're "non-printable" because the auction's volume is reported once, by the `Q`, not order by order.
- **E and D play no part.** E before the cross is pre-market trading; E after is ordinary trading after the open. The D spike is order churn around the open, spread over the whole second and not lined up with any stock's cross.
- **The book's orders supplied only 1.7% of the crossed volume** (344,232 of 19,896,150 shares). The rest came from orders that were never in the displayed book. That fits Nasdaq's opening cross, where special on-open auction orders aren't shown order by order in ITCH, only in aggregate through the imbalance (`I`) messages. Those were published every second until 09:30 and stopped right after. That explanation comes from Nasdaq's rules, not from this file, which only shows that the book didn't supply the other 98.3%.

**What this means for the book builder: no change needed.**
- The handler already applies a C to the order it names, whatever its printable flag, so these orders leave the book correctly.
- It records only printable executions as trades, so the cross isn't double-counted.
- It takes the cross price and volume from the `Q`.
