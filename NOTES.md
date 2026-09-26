# ITCH Feed Handler + Book Builder — planning notes

> Crux of the 13 Sept 2026 conversation. **Not started.** Build in a later session.
> Companion to `../microexchange/`, which is the *venue* side. This is the *participant* side.

---

## 1. What trading firms build in-house

The path a packet travels through a trading firm:

| Stage | What it is | Status |
|---|---|---|
| Feed handler | Parse the exchange's binary protocol (ITCH, MDP3, FAST), millions of msgs/sec | ✗ |
| Book builder | Rebuild the order book from incremental updates — as a **consumer** | ✗ |~
| Strategy / signal | Reads the book, decides | ✗ |
| Pre-trade risk | Position limits, max size, fat-finger. Sub-microsecond, never skipped | ✗ |
| Order gateway | Order state machine, session/sequence handling, OUCH or FIX out | ✗ |
| Matching engine | The venue itself | ✅ `microexchange` |
| Tick store | Columnar time-series DB for research (industry: kdb+) | ✗ |
| Backtester | Event-driven replay of history through a strategy | ✗ |
| Capture & replay | Record the feed, replay deterministically so bugs reproduce | ✗ |
| Async logger | Zero allocation, no I/O on the hot path | ✗ |
| Latency measurement | Histograms, hardware timestamps, PTP clock sync | ✗ |

Real but weak as portfolio projects: reference/symbology data, smart order routing, deployment tooling, PnL keeping.

---

## 2. Venue vs participant — why this is NOT what microexchange already does

```
microexchange    orders ──→ [ I decide ] ──→ trades
                             I own the book — it IS the truth

feed handler     [ Nasdaq decides ] ──→ events ──→ I rebuild a copy
                                                    I own nothing; my copy can be wrong
```

|  | microexchange | feed handler + book builder |
|---|---|---|
| Role | The venue. Decides. | A participant. Observes. |
| Input format | Designed by me (`sizeof(Order)`, memcpy-safe, `static_assert`ed) | Handed to me by a spec |
| Book | The truth | A mirror |
| Key lookup | I have the whole order, so I know its price | **Ref# only** → million-entry hash table on the hot path |
| Instruments | One | ~8,000 interleaved in one stream |
| Failure mode | Matching logic bug | Sequence gap → silently trading on a stale book |
| Verification | I check it myself | Replay a day, compare against Nasdaq's published close |

**Carries over:** price-level layout, the `head`-index trick, contiguous-vector reasoning, benchmark harness shape.
**New:** parsing a format I don't control, order-ID lookup as the dominant cost, gap detection, a correctness oracle.

---

## 3. What ITCH actually looks like

Nasdaq TotalView-ITCH 5.0. **Big-endian. Unaligned. No padding.** Verify every layout against the official spec before writing code.

**Add Order — `'A'`, 36 bytes**
```
offset size field
  0     1   'A'
  1     2   stock locate
  3     2   tracking number
  5     6   timestamp (ns since midnight)   ← 6 bytes, no CPU type matches
 11     8   order reference number
 19     1   'B' / 'S'
 20     4   shares
 24     8   stock (space-padded ASCII)
 32     4   price (4 implied decimals: 1234500 = $123.45)
```

**The messages that matter**

| Msg | Meaning | Payload |
|---|---|---|
| `A` | Add Order | ref#, side, shares, stock, price |
| `E` | Order Executed | **ref#, shares** — no price, no side |
| `X` | Order Cancel | ref#, shares cancelled |
| `D` | Order Delete | **ref# only** |
| `U` | Order Replace | old ref#, new ref#, shares, price |

`E` and `D` carry only a reference number. Finding that order among millions is the core problem.

**Why not `reinterpret_cast` a struct over the buffer:** UB (strict aliasing + alignment). Options: `memcpy` into an integer then `std::byteswap` (C++23), or `std::start_lifetime_as` (C++23). See the C++ deck.

---

## 4. Why market data is UDP multicast — and orders are TCP

### No trade is lost when a packet drops
Market data is a broadcast of things **that already happened**. The trade is inside Nasdaq's engine; it clears and settles regardless. Dropping the packet loses **the news**, not the trade. Response: detect the gap and resync, or stop quoting.

### Both protocols exist, each in the right direction

| Direction | Protocol | Why |
|---|---|---|
| Exchange → everyone (market data) | **UDP multicast** (ITCH) | One-to-many, latency-critical, replaceable |
| Me → exchange (orders) | **TCP** (OUCH, FIX) | Must not be lost, ordered, one-to-one |

### Why the data side is UDP
1. **One-to-many fairness.** TCP = N connections, N copies, and firm #1 gets it before firm #900. Multicast = one packet on the wire, switches replicate it, everyone sees it at essentially the same instant.
2. **TCP head-of-line blocking.** Packet 5 lost, 6–9 arrive → the kernel will not hand me 6–9 until 5 is retransmitted (100µs–1ms). Fresh data held hostage by stale data. With UDP I get 6–9 immediately, see the gap, and **I** decide.
3. **Determinism.** No congestion control, ACK timing, Nagle, retransmit timers.

> **Late data is worse than missing data.** TCP optimises for "eventually complete." Trading needs "now, or tell me you can't."

### What replaces retransmits
- **A feed + B feed** — identical data on two multicast groups over physically separate paths. Take whichever arrives first ("arbitration").
- **TCP retransmit service** for genuine gaps.
- **Snapshot feed** to resync from scratch.

### Not a webhook, not SSE
- Webhook: they HTTP POST to me. They know I exist.
- SSE: long-lived HTTP over TCP, per subscriber.
- Multicast: raw datagrams onto a network segment. **Nasdaq has no idea I exist.** Analogy: radio, or a Kafka topic with no consumer groups, no offsets, no replay.

### Practical constraint
Live multicast requires being on the exchange's network — **colocation**. Not possible from Bangalore. For this project: **historical ITCH files** (same bytes, saved). Nasdaq publishes samples; find the current location myself.

---

## 5. Why anyone needs a book builder

To trade you need the current state of the market. **The book is the input to every decision.**
- **Market making** — place quotes relative to the book; pull them fast when it moves or you get picked off.
- **Book imbalance** — 50k bid vs 500 offered → likely uptick. Read straight off level sizes.
- **Cross-venue** — same stock on Nasdaq, NYSE, BATS, IEX; need every book to see a dislocation.
- **Execution** — fill large orders without moving the price; needs available liquidity now.

Every firm has one. It's the front door.

---

## 6. What makes the project credible, not a toy

1. **A number.** msgs/sec and ns/msg, on stated hardware, methodology written down.
2. **A before/after.** Start naive (`std::map`, `std::unordered_map` for order IDs). Measure. Replace with flat structures / a better hash table. Measure. **The delta is the story.**
3. **A correctness harness.** Replay a full day, verify the book against Nasdaq's end-of-day state.
4. **An honest README.** State what I did *not* do — no kernel bypass, no FPGA, no lock-free claims I can't defend.

---

## 7. Findings about microexchange from the same conversation

Reviewed `src/main.cpp`, `src/Orderbook.h`, `src/TradeLogger.{h,cpp}`.

**Already good**
- Binary input path: one `read()` into a pre-sized `vector<Order>`, `static_assert(is_trivially_copyable)`.
- Text-vs-binary load timing measured separately from match timing.
- Book: contiguous sorted `vector<PriceLevel>`, best at `back()`, `head` index instead of front-erase, compaction threshold.
- Logger: lock held only around the queue push; consumer `swap`s under lock and does file I/O **after** unlocking.

**The latency claim to fix**
"Average is bad but p50/p95/p99 are good" means **rare, very large stalls** — a mean above p99 requires an enormous top 1%. In trading **the tail is what matters**, because slow happens when the market is busy.
→ **Measure p99.9, p99.99 and max.**

**Likely tail sources**
- `std::deque::push_back` can allocate a new chunk **while the matching thread holds the mutex**.
- The matching thread can block on the consumer's lock during the `swap` — brief, but not never.
- Unbounded queue → unbounded memory under a burst.

**Fix:** preallocated SPSC ring buffer — no mutex, no allocation on push.

---

## 8. Candidate projects, ranked

1. **ITCH feed handler + book builder** — new skills; the side every target firm operates. A few weeks.
2. **Backtester / replay simulator** — design-heavy. Hard part is realism: queue position, fill probability, **lookahead bias**.
3. **Async binary logger (SPSC)** — a weekend. Directly improves microexchange's tail. Many exist (NanoLog, Quill), so it's learning more than differentiation. Pairs with learning the memory model / `std::atomic` orderings.
4. **Tick store** — columnar, compressed, mmap'd, range queries on (symbol, time). More data-engineering than latency.

**Suggested order:** logger first (weekend, fixes a measured problem), then feed handler.

> Projects are not the hiring gate at 1–3 yrs — the OA is. This buys concrete "something I optimised" stories and a real education in measurement. Don't let it displace OA prep.

---

## 9. Next session checklist

- [ ] Download and verify the current Nasdaq ITCH 5.0 spec + a sample file
- [ ] Measure microexchange p99.9 / p99.99 / max before touching anything
- [ ] Decide: logger first or feed handler first
- [ ] Feed handler spec: message structs, parse loop, order-ID table design, level storage, benchmark plan, build order
