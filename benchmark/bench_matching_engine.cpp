// benchmark/bench_matching_engine.cpp
// Performance benchmark — measures throughput and latency under load
// Build: cmake --build build --target hft_bench && ./build/hft_bench

#include "core/order_book.hpp"
#include "core/simulator.hpp"
#include "core/types.hpp"
#include "journal/journal.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <numeric>

using namespace hft;
using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
    std::string name;
    int64_t     ops;
    double      total_us;
    double      ops_per_sec;
    double      p50_ns;
    double      p99_ns;
    double      p999_ns;
    double      avg_ns;
};

static BenchResult run_bench(const std::string& name, int64_t ops,
                              std::vector<int64_t>& latencies) {
    std::sort(latencies.begin(), latencies.end());
    double total_ns = 0;
    for (auto l : latencies) total_ns += l;

    BenchResult r;
    r.name        = name;
    r.ops         = ops;
    r.total_us    = total_ns / 1000.0;
    r.ops_per_sec = ops / (total_ns / 1e9);
    r.avg_ns      = total_ns / ops;
    r.p50_ns      = latencies[latencies.size() * 50 / 100];
    r.p99_ns      = latencies[latencies.size() * 99 / 100];
    r.p999_ns     = latencies[latencies.size() * 999 / 1000];
    return r;
}

static void print_result(const BenchResult& r) {
    std::cout << std::left << std::setw(35) << r.name
              << std::right
              << "  ops: " << std::setw(8) << r.ops
              << "  throughput: " << std::setw(10) << std::fixed << std::setprecision(0) << r.ops_per_sec << " ops/s"
              << "  avg: " << std::setw(8) << std::setprecision(1) << r.avg_ns << " ns"
              << "  p50: " << std::setw(8) << r.p50_ns << " ns"
              << "  p99: " << std::setw(8) << r.p99_ns << " ns"
              << "  p999: " << std::setw(8) << r.p999_ns << " ns"
              << "\n";
}

// ─── Benchmark 1: Order Insert (no match) ───────────────────────────────────

static BenchResult bench_insert_no_match(int n) {
    OrderBook book("BENCH");
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> price_dist(1000, 2000);  // spread out
    std::uniform_int_distribution<int> qty_dist(100, 1000);
    std::vector<int64_t> latencies;
    latencies.reserve(n);

    for (int i = 0; i < n; ++i) {
        Order o;
        o.id     = i + 1;
        o.symbol = "BENCH";
        o.side   = (i % 2 == 0) ? Side::BUY : Side::SELL;
        o.type   = OrderType::LIMIT;
        // Spread prices so no matching occurs (buys low, sells high)
        o.price  = (o.side == Side::BUY) ? to_price(100.0 + price_dist(rng) * 0.01)
                                          : to_price(200.0 + price_dist(rng) * 0.01);
        o.qty    = qty_dist(rng);

        auto t0 = Clock::now();
        book.add_order(o);
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    return run_bench("Insert (no match)", n, latencies);
}

// ─── Benchmark 2: Order Match (aggressive orders) ───────────────────────────

static BenchResult bench_match(int n) {
    OrderBook book("BENCH");
    std::mt19937 rng(123);
    std::uniform_int_distribution<int> qty_dist(50, 200);
    std::vector<int64_t> latencies;
    latencies.reserve(n);

    // Pre-populate the book with resting orders
    for (int i = 0; i < n; ++i) {
        Order o;
        o.id     = i + 1;
        o.symbol = "BENCH";
        o.side   = Side::SELL;
        o.type   = OrderType::LIMIT;
        o.price  = to_price(100.0 + (i % 50) * 0.01);  // 50 price levels
        o.qty    = qty_dist(rng);
        book.add_order(o);
    }

    // Now send aggressive buy orders that match
    for (int i = 0; i < n; ++i) {
        Order o;
        o.id     = n + i + 1;
        o.symbol = "BENCH";
        o.side   = Side::BUY;
        o.type   = OrderType::MARKET;
        o.price  = 0;
        o.qty    = 1;  // small qty to match single order partially

        auto t0 = Clock::now();
        book.add_order(o);
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    return run_bench("Match (market order)", n, latencies);
}

// ─── Benchmark 3: Cancel Order ──────────────────────────────────────────────

static BenchResult bench_cancel(int n) {
    OrderBook book("BENCH");
    std::mt19937 rng(456);
    std::uniform_int_distribution<int> price_dist(1, 1000);
    std::vector<int64_t> latencies;
    latencies.reserve(n);

    // Insert orders
    for (int i = 0; i < n; ++i) {
        Order o;
        o.id     = i + 1;
        o.symbol = "BENCH";
        o.side   = Side::BUY;
        o.type   = OrderType::LIMIT;
        o.price  = to_price(100.0 + price_dist(rng) * 0.01);
        o.qty    = 100;
        book.add_order(o);
    }

    // Cancel in random order
    std::vector<int> ids(n);
    std::iota(ids.begin(), ids.end(), 1);
    std::shuffle(ids.begin(), ids.end(), rng);

    for (int id : ids) {
        auto t0 = Clock::now();
        book.cancel_order(id);
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    return run_bench("Cancel (random)", n, latencies);
}

// ─── Benchmark 4: Snapshot ──────────────────────────────────────────────────

static BenchResult bench_snapshot(int n) {
    OrderBook book("BENCH");

    // Build a book with many levels
    for (int i = 0; i < 500; ++i) {
        Order buy, sell;
        buy.id     = i * 2 + 1; buy.symbol = "BENCH"; buy.side = Side::BUY;
        buy.type   = OrderType::LIMIT; buy.price = to_price(100.0 - i * 0.01); buy.qty = 100;

        sell.id    = i * 2 + 2; sell.symbol = "BENCH"; sell.side = Side::SELL;
        sell.type  = OrderType::LIMIT; sell.price = to_price(101.0 + i * 0.01); sell.qty = 100;

        book.add_order(buy);
        book.add_order(sell);
    }

    std::vector<int64_t> latencies;
    latencies.reserve(n);

    for (int i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        auto snap = book.snapshot(10);
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        (void)snap;
    }

    return run_bench("Snapshot (10 levels)", n, latencies);
}

// ─── Benchmark 5: Full Pipeline (submit_order via ExchangeSimulator) ────────

static BenchResult bench_full_pipeline(int n) {
    RiskLimits limits;
    limits.max_order_qty = 100000;
    limits.max_position = 10000000;
    limits.max_notional_usd = 1e12;
    limits.max_orders_per_sec = 1000000;

    ExchangeSimulator sim(limits);
    sim.add_symbol("BENCH");

    std::mt19937 rng(789);
    std::uniform_int_distribution<int> price_dist(9900, 10100);
    std::uniform_int_distribution<int> qty_dist(1, 10);
    std::vector<int64_t> latencies;
    latencies.reserve(n);

    for (int i = 0; i < n; ++i) {
        Order o;
        o.id     = sim.next_order_id();
        o.symbol = "BENCH";
        o.side   = (i % 2 == 0) ? Side::BUY : Side::SELL;
        o.type   = OrderType::LIMIT;
        o.price  = to_price(price_dist(rng) * 0.01);  // ~99.00 - 101.00
        o.qty    = qty_dist(rng);

        auto t0 = Clock::now();
        sim.submit_order(o);
        auto t1 = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    return run_bench("Full pipeline (risk+match)", n, latencies);
}

// ─── Benchmark 6: Backtest Throughput ────────────────────────────────────────

static void bench_backtest_throughput() {
    std::cout << "\n--- Backtest Throughput ---\n";

    int ticks = 50000;
    MarketDataParser::Config cfg;
    cfg.symbol = "BENCH"; cfg.start_price = 100.0;
    cfg.ticks = ticks; cfg.volatility = 0.001; cfg.seed = 42;
    auto tick_data = MarketDataParser::generate_synthetic(cfg);

    RiskLimits rl;
    rl.max_order_qty = 10000;
    rl.max_position = 1000000;
    rl.max_notional_usd = 1e12;
    rl.max_orders_per_sec = 100000;

    auto t0 = Clock::now();

    Backtester bt(rl);
    bt.add_symbol("BENCH");
    bt.set_strategy(std::make_unique<MarketMakingStrategy>(5.0, 100));
    auto result = bt.run(tick_data, false);

    auto t1 = Clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double throughput = ticks / elapsed_s;

    std::cout << "  Ticks:       " << ticks << "\n";
    std::cout << "  Elapsed:     " << std::fixed << std::setprecision(3) << elapsed_s << " s\n";
    std::cout << "  Throughput:  " << std::setprecision(0) << throughput << " ticks/s\n";
    std::cout << "  Orders:      " << result.orders_submitted << "\n";
    std::cout << "  Fills:       " << result.orders_filled << "\n";
    std::cout << "  Trades:      " << result.trades_count << "\n";
    std::cout << "  PnL:         " << std::setprecision(2) << result.total_pnl << "\n";
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                              Orderflow Replay Engine — Performance Benchmark                                    ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n\n";

    const int N = 100000;

    std::vector<BenchResult> results;
    results.push_back(bench_insert_no_match(N));
    results.push_back(bench_match(N));
    results.push_back(bench_cancel(N));
    results.push_back(bench_snapshot(N));
    results.push_back(bench_full_pipeline(N));

    std::cout << "--- Latency Results (N = " << N << " ops each) ---\n\n";
    for (auto& r : results) print_result(r);

    bench_backtest_throughput();

    std::cout << "\n--- Memory ---\n";
    std::cout << "  sizeof(Order):           " << sizeof(Order) << " bytes\n";
    std::cout << "  sizeof(Trade):           " << sizeof(Trade) << " bytes\n";
    std::cout << "  sizeof(PriceLevel):      " << sizeof(PriceLevel) << " bytes\n";
    std::cout << "  sizeof(MarketDataTick):  " << sizeof(MarketDataTick) << " bytes\n";

    std::cout << "\nDone.\n\n";
    return 0;
}
