#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>
#include "orderbook/engine.hpp"

using namespace orderbook;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Synthetic order-flow generator
//
// Distribution modelled on real exchange data:
//   60% passive limit orders (resting, spread across a tight price band)
//   20% aggressive limit orders (cross the spread → immediate fills)
//   10% market orders
//   10% cancel requests (randomly pick a live order ID)
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    const std::size_t N = (argc > 1) ? std::stoull(argv[1]) : 1'000'000;

    Engine engine;
    std::mt19937_64 rng(0xDEADBEEF);

    std::uniform_int_distribution<int>            side_d(0, 1);
    std::uniform_int_distribution<std::int64_t>   passive_px(95, 105);
    std::uniform_int_distribution<std::uint64_t>  qty_d(1, 100);
    std::uniform_int_distribution<int>            class_d(0, 9);

    std::vector<std::uint64_t> live_ids;   // IDs currently resting (for cancel selection)
    live_ids.reserve(N / 2);

    std::uint64_t next_id = 1;

    // Per-op latency in nanoseconds.
    std::vector<long long> latencies;
    latencies.reserve(N);

    // Warm-up: populate the book with a handful of passive orders so early
    // market orders have something to match against.
    for (int i = 0; i < 200; ++i) {
        NewOrder o{};
        o.id        = OrderId{next_id++};
        o.side      = side_d(rng) ? Side::Buy : Side::Sell;
        o.type      = OrderType::Limit;
        o.tif       = TimeInForce::GTC;
        o.price     = Price{passive_px(rng)};
        o.quantity  = Quantity{qty_d(rng)};
        o.timestamp = Timestamp{0};
        auto evts = engine.submit(o);
        for (auto& e : evts)
            if (e.type == EventType::Ack)
                live_ids.push_back(static_cast<std::uint64_t>(o.id));
    }

    // ---- timed loop --------------------------------------------------------
    auto wall_start = high_resolution_clock::now();

    for (std::size_t i = 0; i < N; ++i) {
        auto t0 = high_resolution_clock::now();

        int cls = class_d(rng);

        if (cls < 6) {
            // ---- passive limit (GTC) ---------------------------------------
            NewOrder o{};
            o.id        = OrderId{next_id++};
            o.side      = side_d(rng) ? Side::Buy : Side::Sell;
            o.type      = OrderType::Limit;
            o.tif       = TimeInForce::GTC;
            o.price     = Price{passive_px(rng)};
            o.quantity  = Quantity{qty_d(rng)};
            o.timestamp = Timestamp{i};
            auto evts = engine.submit(o);
            for (auto& e : evts)
                if (e.type == EventType::Ack)
                    live_ids.push_back(next_id - 1);

        } else if (cls < 8) {
            // ---- aggressive limit (IOC, crosses spread) --------------------
            NewOrder o{};
            o.id       = OrderId{next_id++};
            o.side     = side_d(rng) ? Side::Buy : Side::Sell;
            o.type     = OrderType::Limit;
            o.tif      = TimeInForce::IOC;
            // Price chosen to cross: buy at 110, sell at 90.
            o.price    = Price{(o.side == Side::Buy) ? 110LL : 90LL};
            o.quantity = Quantity{qty_d(rng)};
            o.timestamp = Timestamp{i};
            (void)engine.submit(o);

        } else if (cls < 9) {
            // ---- market order ----------------------------------------------
            NewOrder o{};
            o.id       = OrderId{next_id++};
            o.side     = side_d(rng) ? Side::Buy : Side::Sell;
            o.type     = OrderType::Market;
            o.tif      = TimeInForce::IOC;
            o.price    = Price{0};
            o.quantity = Quantity{qty_d(rng)};
            o.timestamp = Timestamp{i};
            (void)engine.submit(o);

        } else {
            // ---- cancel ----------------------------------------------------
            if (!live_ids.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live_ids.size() - 1);
                std::size_t idx = pick(rng);
                std::uint64_t cid = live_ids[idx];
                live_ids[idx] = live_ids.back();
                live_ids.pop_back();
                (void)engine.cancel(OrderId{cid}, Timestamp{i});
            }
        }

        auto t1 = high_resolution_clock::now();
        latencies.push_back(duration_cast<nanoseconds>(t1 - t0).count());
    }

    auto wall_end = high_resolution_clock::now();

    // ---- results -----------------------------------------------------------
    double elapsed_s  = duration_cast<nanoseconds>(wall_end - wall_start).count() * 1e-9;
    double throughput = N / elapsed_s;

    std::sort(latencies.begin(), latencies.end());
    auto p50  = latencies[N * 50  / 100];
    auto p99  = latencies[N * 99  / 100];
    auto p999 = latencies[N * 999 / 1000];

    std::cout << "============================================================\n"
              << "  Matching Engine Benchmark  (" << N / 1'000'000.0 << "M ops)\n"
              << "============================================================\n"
              << "  Elapsed:     " << elapsed_s * 1000.0 << " ms\n"
              << "  Throughput:  " << throughput / 1e6   << " M ops/sec\n"
              << "------------------------------------------------------------\n"
              << "  Latency (per op, including chrono overhead ~10 ns):\n"
              << "    p50  : " << p50  << " ns\n"
              << "    p99  : " << p99  << " ns\n"
              << "    p99.9: " << p999 << " ns\n"
              << "============================================================\n";

    return 0;
}
