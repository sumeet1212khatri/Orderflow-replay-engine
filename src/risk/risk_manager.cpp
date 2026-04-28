#include "risk/risk_manager.hpp"
#include <cmath>
#include <algorithm>

namespace hft {

RiskManager::RiskManager(RiskLimits limits) : limits_(std::move(limits)) {}

int RiskManager::orders_in_last_second() {
    int64_t now    = now_ns();
    int64_t cutoff = now - 1'000'000'000LL;
    while (!order_timestamps_ns_.empty() && order_timestamps_ns_.front() < cutoff)
        order_timestamps_ns_.pop_front();
    return (int)order_timestamps_ns_.size();
}

RiskResult RiskManager::check_order(const Order& order, double current_price) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++stats_checked_;

    if (!limits_.enabled) {
        order_timestamps_ns_.push_back(now_ns());
        return {true, RiskRejectReason::NONE, "OK"};
    }

    if (order.qty <= 0)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::INVALID_QTY, "Quantity must be positive"});

    if (order.type == OrderType::LIMIT && order.price <= 0)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::INVALID_PRICE, "Limit price must be positive"});

    if (order.qty > limits_.max_order_qty)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::MAX_ORDER_QTY,
            "Order qty " + std::to_string(order.qty) + " > limit " + std::to_string(limits_.max_order_qty)});

    double price_usd = (current_price > 0) ? current_price : from_price(order.price);
    double notional  = price_usd * order.qty;
    if (notional > limits_.max_notional_usd)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::MAX_NOTIONAL,
            "Notional $" + std::to_string(notional) + " > limit $" + std::to_string(limits_.max_notional_usd)});

    auto pit         = positions_.find(order.symbol);
    Quantity cur_pos = (pit != positions_.end()) ? pit->second.net_qty : 0;
    Quantity projected = cur_pos + (order.side == Side::BUY ? order.qty : -order.qty);
    if (std::abs(projected) > limits_.max_position)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::MAX_POSITION,
            "Projected position " + std::to_string(projected) + " > limit " + std::to_string(limits_.max_position)});

    // Rate check — only count approved orders toward the rate limit
    int rate = orders_in_last_second();
    if (rate >= limits_.max_orders_per_sec)
        return (++stats_rejected_, RiskResult{false, RiskRejectReason::MAX_ORDER_RATE,
            "Order rate " + std::to_string(rate) + "/s >= limit " + std::to_string(limits_.max_orders_per_sec)});

    // All checks passed — record timestamp for rate limiting
    order_timestamps_ns_.push_back(now_ns());
    return {true, RiskRejectReason::NONE, "OK"};
}

void RiskManager::on_fill(const std::string& symbol, Side side, Quantity qty, double fill_price) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto& pos  = positions_[symbol];
    pos.symbol = symbol;

    int64_t signed_qty = (side == Side::BUY) ? (int64_t)qty : -(int64_t)qty;
    bool adding = (pos.net_qty == 0) ||
                  (side == Side::BUY  && pos.net_qty > 0) ||
                  (side == Side::SELL && pos.net_qty < 0);

    if (adding) {
        double total_cost = pos.avg_price * std::abs((double)pos.net_qty) + fill_price * qty;
        pos.net_qty += signed_qty;
        pos.avg_price = (pos.net_qty != 0)
            ? total_cost / std::abs((double)pos.net_qty)
            : 0.0;
    } else {
        Quantity closing = std::min(qty, (Quantity)std::abs(pos.net_qty));
        Quantity opening = qty - closing;

        double realized = (fill_price - pos.avg_price) * closing *
                          (pos.net_qty > 0 ? 1.0 : -1.0);
        pos.realized_pnl += realized;
        pos.net_qty += signed_qty;

        if (pos.net_qty == 0) {
            pos.avg_price = 0.0;
        } else if (opening > 0) {
            pos.avg_price = fill_price;
        }
    }

    if (pos.net_qty != 0) {
        double mkt = market_prices_.count(symbol) ? market_prices_.at(symbol) : fill_price;
        pos.unrealized_pnl = (mkt - pos.avg_price) * pos.net_qty;
    } else {
        pos.unrealized_pnl = 0.0;
    }
}

void RiskManager::update_market_price(const std::string& sym, double px) {
    std::lock_guard<std::mutex> lock(mtx_);
    market_prices_[sym] = px;
    if (positions_.count(sym)) {
        auto& pos = positions_[sym];
        pos.unrealized_pnl = (pos.net_qty != 0)
            ? (px - pos.avg_price) * pos.net_qty
            : 0.0;
    }
}

void RiskManager::on_cancel(const std::string& symbol, const Order& order) {
    (void)symbol; (void)order;
}

Position RiskManager::get_position(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = positions_.find(symbol);
    if (it == positions_.end()) return Position{symbol, 0, 0, 0, 0};
    return it->second;
}

std::unordered_map<std::string, Position> RiskManager::all_positions() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return positions_;
}

double RiskManager::total_pnl() const {
    std::lock_guard<std::mutex> lock(mtx_);
    double total = 0;
    for (auto& [sym, pos] : positions_)
        total += pos.realized_pnl + pos.unrealized_pnl;
    return total;
}

} // namespace hft
