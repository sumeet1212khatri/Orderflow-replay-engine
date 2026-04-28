#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>

namespace hft {

using Price     = int64_t;   // price in ticks (e.g. 1234567 = $123.4567)
using Quantity  = int64_t;
using OrderId   = uint64_t;
using Timestamp = int64_t;   // nanoseconds since epoch

constexpr Price PRICE_SCALE = 10000; // 4 decimal places

inline Price to_price(double d) { return static_cast<Price>(std::llround(d * PRICE_SCALE)); }
inline double from_price(Price p) { return static_cast<double>(p) / PRICE_SCALE; }

enum class Side : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1, IOC = 2, FOK = 3 };
enum class OrderStatus : uint8_t {
    PENDING, NEW, PARTIALLY_FILLED, FILLED, CANCELLED, REJECTED, EXPIRED
};
enum class TimeInForce : uint8_t { GTC = 0, IOC = 1, FOK = 2, DAY = 3 };

struct Order {
    OrderId    id         = 0;
    std::string symbol;
    Side       side       = Side::BUY;
    OrderType  type       = OrderType::LIMIT;
    Price      price      = 0;      
    Quantity   qty        = 0;
    Quantity   filled_qty = 0;
    OrderStatus status    = OrderStatus::PENDING;
    Timestamp  timestamp  = 0;
    std::string client_id;

    Quantity remaining() const { return qty - filled_qty; }
    bool is_done() const {
        return status == OrderStatus::FILLED ||
               status == OrderStatus::CANCELLED ||
               status == OrderStatus::REJECTED ||
               status == OrderStatus::EXPIRED;
    }
};

struct Trade {
    uint64_t   trade_id;
    OrderId    buy_order_id;
    OrderId    sell_order_id;
    std::string symbol;
    Price      price;
    Quantity   qty;
    Timestamp  timestamp;
};

struct MarketDataTick {
    Timestamp  timestamp;
    std::string symbol;
    Price      bid_price;
    Quantity   bid_qty;
    Price      ask_price;
    Quantity   ask_qty;
    Price      last_price;
    Quantity   last_qty;
};

struct L2Level {
    Price    price;
    Quantity qty;
    int      order_count;
};

struct OrderBookSnapshot {
    std::string symbol;
    Timestamp   timestamp;
    std::vector<L2Level> bids; 
    std::vector<L2Level> asks; 
    Price best_bid() const { return bids.empty() ? 0 : bids[0].price; }
    Price best_ask() const { return asks.empty() ? 0 : asks[0].price; }
    Price mid()      const {
        if (bids.empty() || asks.empty()) return 0;
        return (best_bid() + best_ask()) / 2;
    }
    double spread()  const {
        if (bids.empty() || asks.empty()) return 0.0;
        Price s = best_ask() - best_bid();
        return s > 0 ? from_price(s) : 0.0;
    }
};

struct RiskLimits {
    Quantity max_order_qty     = 10000;
    Quantity max_position      = 100000;
    double   max_notional_usd  = 1000000.0;
    int      max_orders_per_sec = 1000;
    bool     enabled           = true;
};

struct Position {
    std::string symbol;
    Quantity    net_qty   = 0;  
    double      avg_price = 0.0;
    double      realized_pnl  = 0.0;
    double      unrealized_pnl = 0.0;
    double      notional() const { return std::abs(net_qty * avg_price); }
};

struct LatencyStats {
    int64_t count   = 0;
    int64_t sum_ns  = 0;
    int64_t min_ns  = INT64_MAX;
    int64_t max_ns  = 0;
    int64_t p50_ns  = 0;
    int64_t p99_ns  = 0;
    std::vector<int64_t> samples; 

    void record(int64_t ns) {
        ++count; sum_ns += ns;
        if (ns < min_ns) min_ns = ns;
        if (ns > max_ns) max_ns = ns;
        samples.push_back(ns);
    }
    void compute_percentiles() {
        if (samples.empty()) return;
        std::vector<int64_t> sorted = samples; // Fix: Copy before sort
        std::sort(sorted.begin(), sorted.end());
        p50_ns = sorted[sorted.size() * 50 / 100];
        p99_ns = sorted[sorted.size() * 99 / 100];
    }
    double avg_us() const { return count ? (double)sum_ns / count / 1000.0 : 0; }
};

inline Timestamp now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

} // namespace hft
