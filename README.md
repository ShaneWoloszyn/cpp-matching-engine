# C++ Matching Engine

A limit order book matching engine written in C++. This is the core component of any exchange — it takes buy and sell orders, matches them against each other, and tracks everything that's still waiting to be filled.

Handles limit orders, market orders, and IOC/FOK orders. Supports partial fills, order cancellation, and price-time priority (better price wins; same price goes to whoever arrived first).

Throughput: **2.43M orders/sec** on a single thread (p50 latency 300ns).

---

## How it works

The book is two sorted maps — one for bids (descending), one for asks (ascending). Each price level holds a FIFO queue of resting orders. A separate hashmap stores an iterator into that queue for every live order, which is what makes cancellation O(1) instead of O(N).

When an order comes in, it walks the opposite side of the book level by level, consuming resting orders until it's filled or there's nothing left to match against. Whatever's left either rests on the book (GTC), gets cancelled (IOC), or rejects the whole thing (FOK).

---

## Build

Requires GCC 11+ or Clang 13+, CMake 3.20+, Python 3.9+. pybind11 is fetched automatically.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cmake --install build
```

---

## Tests

```bash
pytest tests/ -v
```

29 tests covering resting orders, full/partial fills, price-time priority, IOC/FOK/Market, cancellation, duplicate IDs, validation, and a 5000-op stress test with invariant checks.

---

## Benchmark

```bash
./build/bench
```

Mix of 60% passive limits, 20% aggressive limits, 10% market orders, 10% cancels.

```
Elapsed:     411 ms
Throughput:  2.43 M ops/sec

Latency:
  p50  : 300 ns
  p99  : 900 ns
  p99.9: 1700 ns
```

---

## What's slow and what comes next

The two main bottlenecks in v1:

**std::map pointer chasing** — a red-black tree means every price level lookup is walking pointers across cache lines. Replacing it with a flat array indexed by price tick would cut this significantly for instruments with a bounded price range.

**Per-order heap allocation** — std::list allocates each node individually. Pre-allocating a pool of order structs and handing them out from a free list would eliminate most of that overhead.

---

## API

```cpp
engine.submit(order)   // returns vector of events (Ack, Fill, Cancel, Reject)
engine.cancel(id, ts)  // cancel a resting order by ID
engine.best_bid()      // top bid level
engine.best_ask()      // top ask level
engine.bid_depth(n)    // top N bid levels
engine.ask_depth(n)    // top N ask levels
```
