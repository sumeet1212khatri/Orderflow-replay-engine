#pragma once
#include "core/types.hpp"
#include <unordered_map>
#include <string>
#include <deque>
#include <mutex>

namespace hft {

enum class RiskRejectReason {
    NONE,
    MAX_ORDER_QTY,
    MAX_POSITION,
    MAX_NOTIONAL,
    MAX_ORDER_RATE,
    INVALID_PRICE,
    INVALID_QTY,
    SYMBOL_NOT_ALLOWED
};

struct RiskResult {
    bool   approved = false;
    RiskRejectReason reason = RiskRejectReason::NONE;
    std::string message;
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits);

    RiskResult check_order(const Order& order, double current_price = 0.0);
    void on_fill(const std::string& symbol, Side side, Quantity qty, double fill_price);
    void on_cancel(const std::string& symbol, const Order& order);

    Position get_position(const std::string& symbol) const;
    std::unordered_map<std::string, Position> all_positions() const;
    double total_pnl() const;

    void set_limits(RiskLimits l) { limits_ = l; }
    RiskLimits get_limits() const { return limits_; }
    void update_market_price(const std::string& sym, double px);

    // Stats
    int64_t orders_checked()   const { return stats_checked_; }
    int64_t orders_rejected()  const { return stats_rejected_; }

private:
    RiskLimits limits_;
    mutable std::mutex mtx_;

    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double>   market_prices_;

    // Order rate tracking (sliding window 1 sec)
    std::deque<int64_t> order_timestamps_ns_;

    int64_t stats_checked_  = 0;
    int64_t stats_rejected_ = 0;

    int orders_in_last_second();
};

} // namespace hft
