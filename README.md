# C++ Limit Order Book Matching Engine

A single-threaded, in-process matching engine written in C++17.  
Accepts limit, market, and IOC/FOK orders; produces price-time priority matching with partial fills, cancellation by ID, and Python bindings for testing.

**Benchmark (Windows, MinGW GCC 15.1, -O3):**  
`2.43 M ops/sec` — `p50 300 ns / p99 900 ns / p99.9 1 700 ns` per operation

---

## What it does

| Feature | Detail |
|---|---|
| Order types | Limit, Market |
| Time-in-force | GTC, IOC, FOK, Day |
| Matching rule | Price-time priority (best price first; FIFO within a level) |
| Partial fills | Yes — an order can fill against multiple resting orders |
| Cancellation | O(1) via hashmap-to-list-iterator locator |
| Book query | `best_bid()`, `best_ask()`, `bid_depth(n)`, `ask_depth(n)` |
| Events emitted | Ack, Fill (both sides), Cancel, Reject with typed reasons |

---

## Data structure design

```
Engine
├── bids: std::map<Price, PriceLevel, std::greater>   ← descending; best bid = begin()
├── asks: std::map<Price, PriceLevel, std::less>      ← ascending;  best ask = begin()
│         PriceLevel { std::list<Order> queue; Quantity total; }
└── loc:  std::unordered_map<OrderId, Locator>        ← O(1) cancel
          Locator { side, price, std::list<Order>::iterator }
```

**Why `std::map` for price levels:** the book is sparse — real instruments trade at
tens to hundreds of distinct prices, not millions. The tree overhead is tiny compared to
the dominant cost of list iteration during matching.

**Why `std::list` for the FIFO queue:** list iterators are stable across insertions and
deletions, which is exactly what the locator hashmap relies on for O(1) cancel. A
`std::deque` would need index arithmetic to stay valid; a `std::vector` invalidates all
iterators on erase. The list's per-node allocation is a known cost (see §Optimization
below).

**Locator invariant:** every entry in `loc` has a valid iterator into the corresponding
`bids` or `asks` list. `assert_invariants()` verifies this in O(N) — called by the test
suite, never on the hot path.

---

## Build

**Requirements:** GCC ≥ 11 (or Clang ≥ 13 / MSVC 2022), CMake ≥ 3.20, Python ≥ 3.9,
pybind11 (auto-fetched by CMake).

```bash
# Configure (fetches pybind11 from GitHub on first run)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build everything: engine lib, bench binary, Python extension
cmake --build build -j$(nproc)

# Optional: copy the .pyd/.so next to the tests so pytest finds it automatically
cmake --install build
```

On Windows with MSYS2/MinGW:
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build -j4
```

---

## Tests

```bash
# After cmake --install (copies orderbook.pyd next to tests/)
pytest tests/ -v

# Or point PYTHONPATH at the build output directly
PYTHONPATH=build pytest tests/ -v
```

29 tests covering: resting, full/partial fills, price-time priority, IOC, FOK,
market orders, cancellation, duplicate IDs, validation, depth ordering, and a
5 000-operation stress test with invariant checks every 100 ops.

---

## Benchmark

```bash
./build/bench [N]   # N = number of ops, default 1 000 000
```

Synthetic workload (models empirical exchange order flow):
- 60% passive limit GTC — spread across a 10-tick price band (95–105)
- 20% aggressive limit IOC — priced to cross, generating immediate fills
- 10% market IOC — fill at whatever the best price is
- 10% cancel — randomly select a live resting order

Results on the dev machine (AMD Ryzen, Windows 11, MinGW GCC 15.1 `-O3`):

```
============================================================
  Matching Engine Benchmark  (1M ops)
============================================================
  Elapsed:     411.749 ms
  Throughput:  2.43 M ops/sec
------------------------------------------------------------
  Latency (per op, including chrono overhead ~10 ns):
    p50  : 300 ns
    p99  : 900 ns
    p99.9: 1700 ns
============================================================
```

---

## v1 hot-path analysis and next optimizations

The v1 design is deliberately straightforward. Profiling (`perf` on Linux / VTune on
Windows) will likely point to two bottlenecks:

### 1. `std::map` pointer chasing
`std::map` is a red-black tree. Traversing it walks pointers across cache lines.
Replacement options:

| Structure | When to prefer |
|---|---|
| `std::vector<{Price,PriceLevel}>` sorted | Book has few, slowly-changing levels |
| Array indexed by `price - min_price` | Prices are dense in a known bounded range (tick data) |
| Intrusive skip-list or B-tree | Wide price range, frequent insert/delete |

### 2. Per-node heap allocation in `std::list`
Each `list::push_back` calls `new`. At 2M orders/sec that's 2M allocations/sec.
Fix: object pool — pre-allocate a flat buffer of `Order` structs, hand them out via a
free-list. The pool is cache-friendly; allocations are O(1) with zero heap traffic.

### 3. Event vector allocation
`submit()` returns `std::vector<Event>`. Every call heap-allocates. Use a small-buffer
optimization or pass a caller-owned output range instead.

---

## API reference

```cpp
namespace orderbook {

struct NewOrder { OrderId id; Side side; OrderType type; TimeInForce tif;
                  Price price; Quantity quantity; Timestamp timestamp; };

struct Event    { EventType type; OrderId order_id; OrderId counterparty;
                  Price price; Quantity quantity; Timestamp timestamp;
                  RejectReason reject_reason; CancelReason cancel_reason; };

class Engine {
    std::vector<Event> submit(const NewOrder&);
    std::vector<Event> cancel(OrderId, Timestamp);
    std::optional<TopOfBook> best_bid() const noexcept;
    std::optional<TopOfBook> best_ask() const noexcept;
    std::vector<Level>       bid_depth(size_t n) const;
    std::vector<Level>       ask_depth(size_t n) const;
    void assert_invariants() const;  // O(N); tests only
};

}
```

Events returned per operation:

| Scenario | Events |
|---|---|
| Limit GTC, no cross | `[Ack]` |
| Limit/Market, full fill | `[Fill×2 per passive order]` |
| Limit GTC, partial fill | `[Fill×2 per passive, Ack(remainder)]` |
| IOC, partial fill | `[Fill×2 per passive, Cancel(remainder)]` |
| IOC / Market, zero fill | `[Reject(IocNoFill)]` |
| FOK, insufficient liquidity | `[Reject(FokInsufficientLiquidity)]` |
| Cancel hit | `[Cancel(UserRequested)]` |
| Cancel miss / duplicate ID / bad params | `[Reject(...)]` |
