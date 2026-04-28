#include "core/simulator.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <unordered_set>

namespace hft {

ExchangeSimulator::ExchangeSimulator(RiskLimits limits)
    : risk_(limits), journal_("session.journal") {}

void ExchangeSimulator::add_symbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (books_.count(symbol)) return;
    auto book = std::make_unique<OrderBook>(symbol);
    book->set_trade_callback([this](const Trade& t) {
        total_trades_.fetch_add(1, std::memory_order_relaxed);
        if (trade_cb_) trade_cb_(t);
    });
    book->set_order_callback([this](const Order& o) {
        if (order_cb_) order_cb_(o);
    });
    books_[symbol] = std::move(book);
}

// Fix: Now takes const Order& and safely clones it to guarantee memory persistence
ExchangeSimulator::SubmitResult ExchangeSimulator::submit_order(const Order& order_in, double market_price) {
    int64_t t0 = now_ns();
    total_orders_.fetch_add(1, std::memory_order_relaxed);

    auto risk_result = risk_.check_order(order_in, market_price);
    if (!risk_result.approved) {
        total_rejects_.fetch_add(1, std::memory_order_relaxed);
        return {false, risk_result.message, {}};
    }

    std::lock_guard<std::mutex> lock(mtx_);
    auto it = books_.find(order_in.symbol);
    if (it == books_.end()) {
        total_rejects_.fetch_add(1, std::memory_order_relaxed);
        return {false, "Unknown symbol: " + order_in.symbol, {}};
    }

    // Fix: Allocate order uniquely so OrderBook holds a stable, valid pointer
    auto order_ptr = std::make_unique<Order>(order_in);
    order_ptr->timestamp = now_ns();
    Order& order = *order_ptr;
    order_store_[order.id] = std::move(order_ptr);

    auto trades = it->second->add_order(order);

    for (auto& t : trades) {
        Side fill_side = (t.buy_order_id == order.id) ? Side::BUY : Side::SELL;
        risk_.on_fill(t.symbol, fill_side, t.qty, from_price(t.price));
    }

    // Fix: Recorded inside the mutex scope to prevent vector corruption
    order_lat_.record(now_ns() - t0);

    return {true, "OK", trades};
}

bool ExchangeSimulator::cancel_order(const std::string& symbol, OrderId id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return false;
    return it->second->cancel_order(id);
}

bool ExchangeSimulator::modify_order(const std::string& symbol, OrderId id,
                                      Price new_price, Quantity new_qty) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return false;
    return it->second->modify_order(id, new_price, new_qty);
}

void ExchangeSimulator::on_tick(const MarketDataTick& tick) {
    int64_t t0 = now_ns();
    std::lock_guard<std::mutex> lock(mtx_);
    risk_.update_market_price(tick.symbol, from_price(tick.last_price));
    tick_lat_.record(now_ns() - t0);
}

OrderBookSnapshot ExchangeSimulator::get_snapshot(const std::string& symbol, int depth) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return {};
    return it->second->snapshot(depth);
}

std::vector<std::string> ExchangeSimulator::symbols() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> s;
    for (auto& [k, _] : books_) s.push_back(k);
    return s;
}

Position ExchangeSimulator::get_position(const std::string& symbol) const {
    return risk_.get_position(symbol);
}

std::unordered_map<std::string, Position> ExchangeSimulator::all_positions() const {
    return risk_.all_positions();
}

double ExchangeSimulator::total_pnl() const {
    return risk_.total_pnl();
}

Backtester::Backtester(RiskLimits limits) : limits_(limits) {}

void Backtester::set_strategy(std::unique_ptr<IStrategy> strategy) {
    strategy_ = std::move(strategy);
}

void Backtester::add_symbol(const std::string& symbol) {
    symbols_.push_back(symbol);
}

void Backtester::process_signals(const std::vector<StrategySignal>& sigs,
                                  const MarketDataTick& tick,
                                  BacktestResult& result,
                                  std::unordered_map<OrderId, Order>& live_orders) {
    for (auto& sig : sigs) {
        if (sig.action == StrategySignal::Action::NONE) continue;

        if (sig.action == StrategySignal::Action::CANCEL) {
            sim_->cancel_order(tick.symbol, sig.cancel_id);
            live_orders.erase(sig.cancel_id);
            continue;
        }
        if (sig.action == StrategySignal::Action::CANCEL_ALL) {
            for (auto& [id, ord] : live_orders)
                sim_->cancel_order(ord.symbol, id);
            live_orders.clear();
            continue;
        }

        Order order;
        order.id        = sim_->next_order_id();
        order.symbol    = sig.symbol.empty() ? tick.symbol : sig.symbol;
        order.side      = (sig.action == StrategySignal::Action::BUY) ? Side::BUY : Side::SELL;
        order.type      = sig.type;
        order.price     = sig.price;
        order.qty       = sig.qty;
        order.client_id = sig.reason;

        ++result.orders_submitted;
        auto res = sim_->submit_order(order, from_price(tick.last_price));

        if (!res.approved) {
            ++result.orders_rejected;
        } else {
            if (!order.is_done()) live_orders[order.id] = order;
            for (auto& t : res.trades) {
                ++result.orders_filled;
                ++result.trades_count;
                result.trades.push_back(t);
                strategy_->on_trade(t);
            }
            strategy_->on_order_update(order);
        }
    }
}

BacktestResult Backtester::run(const std::vector<MarketDataTick>& ticks,
                                bool enable_journal,
                                const std::string& journal_path) {
    BacktestResult result;
    if (!strategy_ || ticks.empty()) return result;
    result.strategy_name = strategy_->name();

    sim_ = std::make_unique<ExchangeSimulator>(limits_);
    for (auto& sym : symbols_) sim_->add_symbol(sym);
    if (symbols_.empty() && !ticks.empty()) {
        std::unordered_map<std::string, bool> seen;
        for (auto& t : ticks) {
            if (!seen[t.symbol]) {
                sim_->add_symbol(t.symbol);
                seen[t.symbol] = true;
            }
        }
    }

    std::unordered_set<std::string> inited;
    for (auto& tick : ticks) {
        if (!inited.count(tick.symbol)) {
            strategy_->on_init(tick.symbol);
            inited.insert(tick.symbol);
        }
    }

    std::unordered_map<OrderId, Order> live_orders;
    double peak_equity = std::numeric_limits<double>::lowest();
    int64_t ts_start   = now_ns();

    for (auto& tick : ticks) {
        int64_t t0 = now_ns();
        sim_->on_tick(tick);

        auto snap = sim_->get_snapshot(tick.symbol);
        auto sigs = strategy_->on_tick(tick, snap);

        process_signals(sigs, tick, result, live_orders);

        if (result.ticks_processed % 100 == 0) {
            double equity = sim_->total_pnl();
            result.equity_curve.push_back(equity);
            if (equity > peak_equity) peak_equity = equity;
            if (peak_equity > std::numeric_limits<double>::lowest()) {
                double drawdown = peak_equity - equity;
                if (drawdown > result.max_drawdown) result.max_drawdown = drawdown;
            }
        }

        result.tick_latency.record(now_ns() - t0);
        ++result.ticks_processed;
    }

    int64_t elapsed_ns  = now_ns() - ts_start;
    result.total_pnl    = sim_->total_pnl();

    auto pos = sim_->all_positions();
    for (auto& [sym, p] : pos) {
        result.realized_pnl   += p.realized_pnl;
        result.unrealized_pnl += p.unrealized_pnl;
    }

    result.order_latency = sim_->order_latency();
    result.order_latency.compute_percentiles();
    result.tick_latency.compute_percentiles();

    double elapsed_s       = elapsed_ns / 1e9;
    result.throughput_eps  = elapsed_s > 0 ? result.ticks_processed / elapsed_s : 0;

    if (result.equity_curve.size() > 2) {
        std::vector<double> returns;
        returns.reserve(result.equity_curve.size() - 1);
        for (size_t i = 1; i < result.equity_curve.size(); ++i)
            returns.push_back(result.equity_curve[i] - result.equity_curve[i - 1]);
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double var  = 0;
        for (double r : returns) var += (r - mean) * (r - mean);
        double std_dev = std::sqrt(var / returns.size());
        result.sharpe = (std_dev > 1e-9) ? (mean / std_dev * std::sqrt(252.0)) : 0;
    }

    return result;
}

} // namespace hft
