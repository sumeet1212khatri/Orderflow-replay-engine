#pragma once
#include "core/types.hpp"
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <deque>
#include <cmath>

namespace hft {

struct StrategySignal {
    enum class Action { NONE, BUY, SELL, CANCEL, CANCEL_ALL } action = Action::NONE;
    std::string symbol;
    OrderType   type      = OrderType::LIMIT;
    Price       price     = 0;
    Quantity    qty       = 0;
    OrderId     cancel_id = 0;
    std::string reason;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual std::string name() const = 0;
    virtual void on_init(const std::string& symbol) { (void)symbol; }
    virtual std::vector<StrategySignal> on_tick(const MarketDataTick& tick,
                                                 const OrderBookSnapshot& book) = 0;
    virtual void on_order_update(const Order& order) { (void)order; }
    virtual void on_trade(const Trade& trade)         { (void)trade; }
    virtual std::string summary() const               { return ""; }
};

class MarketMakingStrategy : public IStrategy {
public:
    explicit MarketMakingStrategy(double spread_bps = 5.0, Quantity qty = 100)
        : spread_bps_(spread_bps), qty_(qty) {}

    std::string name() const override { return "MarketMaking"; }

    std::vector<StrategySignal> on_tick(const MarketDataTick& tick,
                                         const OrderBookSnapshot& book) override {
        std::vector<StrategySignal> sigs;
        if (book.bids.empty() || book.asks.empty()) return sigs;

        double mid  = from_price(book.mid());
        double half = mid * spread_bps_ / 20000.0;

        if (bid_id_) {
            StrategySignal s;
            s.action    = StrategySignal::Action::CANCEL;
            s.cancel_id = bid_id_;
            sigs.push_back(s);
            bid_id_ = 0;
        }
        if (ask_id_) {
            StrategySignal s;
            s.action    = StrategySignal::Action::CANCEL;
            s.cancel_id = ask_id_;
            sigs.push_back(s);
            ask_id_ = 0;
        }

        StrategySignal buy;
        buy.action = StrategySignal::Action::BUY;
        buy.symbol = tick.symbol;
        buy.type   = OrderType::LIMIT;
        buy.price  = to_price(mid - half);
        buy.qty    = qty_;
        buy.reason = "MM_BID";
        sigs.push_back(buy);

        StrategySignal sell;
        sell.action = StrategySignal::Action::SELL;
        sell.symbol = tick.symbol;
        sell.type   = OrderType::LIMIT;
        sell.price  = to_price(mid + half);
        sell.qty    = qty_;
        sell.reason = "MM_ASK";
        sigs.push_back(sell);

        return sigs;
    }

    void on_order_update(const Order& order) override {
        if (order.status == OrderStatus::REJECTED ||
            order.status == OrderStatus::CANCELLED ||
            order.status == OrderStatus::EXPIRED) {
            if (order.id == bid_id_) bid_id_ = 0;
            if (order.id == ask_id_) ask_id_ = 0;
            return;
        }
        if (order.status == OrderStatus::NEW) {
            if (order.side == Side::BUY)  bid_id_ = order.id;
            if (order.side == Side::SELL) ask_id_ = order.id;
        }
        if (order.status == OrderStatus::FILLED) {
            ++fills_;
            if (order.id == bid_id_) bid_id_ = 0;
            if (order.id == ask_id_) ask_id_ = 0;
        }
    }

    std::string summary() const override {
        return "MarketMaking fills=" + std::to_string(fills_);
    }

private:
    double   spread_bps_;
    Quantity qty_;
    OrderId  bid_id_ = 0;
    OrderId  ask_id_ = 0;
    int      fills_  = 0;
};

class MomentumStrategy : public IStrategy {
public:
    explicit MomentumStrategy(int lookback = 20, Quantity qty = 200)
        : lookback_(lookback), qty_(qty) {}

    std::string name() const override { return "Momentum"; }

    std::vector<StrategySignal> on_tick(const MarketDataTick& tick,
                                         const OrderBookSnapshot& book) override {
        std::vector<StrategySignal> sigs;
        double mid = from_price(tick.last_price);
        prices_.push_back(mid);
        if ((int)prices_.size() > lookback_ * 2) prices_.pop_front();
        if ((int)prices_.size() < lookback_) return sigs;

        int n = (int)prices_.size();
        double fast = 0, slow = 0;
        int fast_n = lookback_ / 2;
        for (int i = n - fast_n; i < n; ++i) fast += prices_[i];
        fast /= fast_n;
        for (int i = n - lookback_; i < n; ++i) slow += prices_[i];
        slow /= lookback_;

        if (fast > slow * 1.0002 && !long_) {
            StrategySignal s;
            s.action = StrategySignal::Action::BUY;
            s.symbol = tick.symbol;
            s.type   = OrderType::MARKET;
            s.qty    = qty_;
            s.reason = "MOMENTUM_LONG";
            sigs.push_back(s);
            long_ = true;
            ++signals_;
        } else if (fast < slow * 0.9998 && long_) {
            StrategySignal s;
            s.action = StrategySignal::Action::SELL;
            s.symbol = tick.symbol;
            s.type   = OrderType::MARKET;
            s.qty    = qty_;
            s.reason = "MOMENTUM_FLAT";
            sigs.push_back(s);
            long_ = false;
            ++signals_;
        }
        return sigs;
    }

    std::string summary() const override {
        return "Momentum signals=" + std::to_string(signals_);
    }

private:
    int lookback_;
    Quantity qty_;
    std::deque<double> prices_;
    bool long_    = false;
    int  signals_ = 0;
};

class MeanReversionStrategy : public IStrategy {
public:
    explicit MeanReversionStrategy(int window = 30, double z_entry = 2.0, Quantity qty = 100)
        : window_(window), z_entry_(z_entry), qty_(qty) {}

    std::string name() const override { return "MeanReversion"; }

    std::vector<StrategySignal> on_tick(const MarketDataTick& tick,
                                         const OrderBookSnapshot& book) override {
        std::vector<StrategySignal> sigs;
        double px = from_price(tick.last_price);
        buf_.push_back(px);
        if ((int)buf_.size() > window_) buf_.pop_front();
        if ((int)buf_.size() < window_) return sigs;

        double mean = 0;
        for (double v : buf_) mean += v;
        mean /= window_;

        double var = 0;
        for (double v : buf_) var += (v - mean) * (v - mean);
        double stddev = std::sqrt(var / window_);
        if (stddev < 1e-9) return sigs;

        double z = (px - mean) / stddev;

        if (z < -z_entry_ && !in_trade_) {
            StrategySignal s;
            s.action = StrategySignal::Action::BUY;
            s.symbol = tick.symbol;
            s.type   = OrderType::LIMIT;
            s.price  = tick.ask_price;
            s.qty    = qty_;
            s.reason = "MR_LONG z=" + std::to_string(z).substr(0, 6);
            sigs.push_back(s);
            in_trade_ = true;
        } else if (z > z_entry_ && !in_trade_) {
            StrategySignal s;
            s.action = StrategySignal::Action::SELL;
            s.symbol = tick.symbol;
            s.type   = OrderType::LIMIT;
            s.price  = tick.bid_price;
            s.qty    = qty_;
            s.reason = "MR_SHORT z=" + std::to_string(z).substr(0, 6);
            sigs.push_back(s);
            in_trade_ = true;
        } else if (std::abs(z) < 0.3 && in_trade_) {
            StrategySignal s;
            s.action = StrategySignal::Action::CANCEL_ALL;
            s.reason = "MR_EXIT";
            sigs.push_back(s);
            in_trade_ = false;
        }
        return sigs;
    }

private:
    int      window_;
    double   z_entry_;
    Quantity qty_;
    std::deque<double> buf_;
    bool in_trade_ = false;
};

} // namespace hft
