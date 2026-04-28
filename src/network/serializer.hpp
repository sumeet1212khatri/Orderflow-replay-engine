#pragma once
#include "core/simulator.hpp"
#include "core/types.hpp"
#include <string>
#include <sstream>
#include <iomanip>

namespace hft {

struct J {
    static std::string str(const std::string& s) {
        std::ostringstream o; o << "\"";
        for (char c : s) {
            if (c == '"') o << "\\\"";
            else if (c == '\\') o << "\\\\";
            else o << c;
        }
        o << "\""; return o.str();
    }
    static std::string num(double v, int prec = 4) {
        std::ostringstream o; o << std::fixed << std::setprecision(prec) << v; return o.str();
    }
    static std::string num(int64_t v)  { return std::to_string(v); }
    static std::string num(uint64_t v) { return std::to_string(v); }
    static std::string boolean(bool b) { return b ? "true" : "false"; }
};

inline std::string order_to_json(const Order& o) {
    return "{\"id\":"         + J::num(o.id) +
           ",\"symbol\":"     + J::str(o.symbol) +
           ",\"side\":"       + J::str(o.side == Side::BUY ? "BUY" : "SELL") +
           ",\"type\":"       + J::str([&]{ switch(o.type){
               case OrderType::LIMIT:  return "LIMIT";
               case OrderType::MARKET: return "MARKET";
               case OrderType::IOC:    return "IOC";
               case OrderType::FOK:    return "FOK";
           } return "LIMIT"; }()) +
           ",\"price\":"      + J::num(from_price(o.price)) +
           ",\"qty\":"        + J::num(o.qty) +
           ",\"filled_qty\":" + J::num(o.filled_qty) +
           ",\"status\":"     + J::str([&]{ switch(o.status){
               case OrderStatus::PENDING:          return "PENDING";
               case OrderStatus::NEW:              return "NEW";
               case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
               case OrderStatus::FILLED:           return "FILLED";
               case OrderStatus::CANCELLED:        return "CANCELLED";
               case OrderStatus::REJECTED:         return "REJECTED";
               case OrderStatus::EXPIRED:          return "EXPIRED";
           } return "UNKNOWN"; }()) +
           ",\"timestamp\":"  + J::num(o.timestamp) + "}";
}

inline std::string trade_to_json(const Trade& t) {
    return "{\"trade_id\":"      + J::num(t.trade_id) +
           ",\"buy_order_id\":"  + J::num(t.buy_order_id) +
           ",\"sell_order_id\":" + J::num(t.sell_order_id) +
           ",\"symbol\":"        + J::str(t.symbol) +
           ",\"price\":"         + J::num(from_price(t.price)) +
           ",\"qty\":"           + J::num(t.qty) +
           ",\"timestamp\":"     + J::num(t.timestamp) + "}";
}

inline std::string snapshot_to_json(const OrderBookSnapshot& s, int depth = 10) {
    std::string bids = "[";
    for (int i = 0; i < (int)s.bids.size() && i < depth; ++i) {
        if (i) bids += ",";
        bids += "{\"price\":" + J::num(from_price(s.bids[i].price)) +
                ",\"qty\":"   + J::num(s.bids[i].qty) +
                ",\"orders\":" + J::num((int64_t)s.bids[i].order_count) + "}";
    }
    bids += "]";
    std::string asks = "[";
    for (int i = 0; i < (int)s.asks.size() && i < depth; ++i) {
        if (i) asks += ",";
        asks += "{\"price\":" + J::num(from_price(s.asks[i].price)) +
                ",\"qty\":"   + J::num(s.asks[i].qty) +
                ",\"orders\":" + J::num((int64_t)s.asks[i].order_count) + "}";
    }
    asks += "]";
    return "{\"symbol\":"    + J::str(s.symbol) +
           ",\"timestamp\":" + J::num(s.timestamp) +
           ",\"best_bid\":"  + J::num(from_price(s.best_bid())) +
           ",\"best_ask\":"  + J::num(from_price(s.best_ask())) +
           ",\"mid\":"       + J::num(from_price(s.mid())) +
           ",\"spread\":"    + J::num(s.spread(), 6) +
           ",\"bids\":"      + bids +
           ",\"asks\":"      + asks + "}";
}

inline std::string position_to_json(const Position& p) {
    return "{\"symbol\":"         + J::str(p.symbol) +
           ",\"net_qty\":"        + J::num(p.net_qty) +
           ",\"avg_price\":"      + J::num(p.avg_price) +
           ",\"realized_pnl\":"   + J::num(p.realized_pnl) +
           ",\"unrealized_pnl\":" + J::num(p.unrealized_pnl) +
           ",\"notional\":"       + J::num(p.notional()) + "}";
}

inline std::string backtest_result_to_json(const BacktestResult& r) {
    std::string equity = "[";
    for (size_t i = 0; i < r.equity_curve.size(); ++i) {
        if (i) equity += ",";
        equity += J::num(r.equity_curve[i]);
    }
    equity += "]";
    std::string trades_str = "[";
    for (size_t i = 0; i < r.trades.size() && i < 500; ++i) {
        if (i) trades_str += ",";
        trades_str += trade_to_json(r.trades[i]);
    }
    trades_str += "]";
    return "{\"strategy\":"        + J::str(r.strategy_name) +
           ",\"ticks_processed\":" + J::num(r.ticks_processed) +
           ",\"orders_submitted\":" + J::num(r.orders_submitted) +
           ",\"orders_filled\":"   + J::num(r.orders_filled) +
           ",\"orders_rejected\":" + J::num(r.orders_rejected) +
           ",\"trades_count\":"    + J::num(r.trades_count) +
           ",\"total_pnl\":"       + J::num(r.total_pnl) +
           ",\"realized_pnl\":"    + J::num(r.realized_pnl) +
           ",\"unrealized_pnl\":"  + J::num(r.unrealized_pnl) +
           ",\"max_drawdown\":"    + J::num(r.max_drawdown) +
           ",\"sharpe\":"          + J::num(r.sharpe) +
           ",\"order_p50_us\":"    + J::num(r.order_latency.p50_ns / 1000.0) +
           ",\"order_p99_us\":"    + J::num(r.order_latency.p99_ns / 1000.0) +
           ",\"order_avg_us\":"    + J::num(r.order_latency.avg_us()) +
           ",\"tick_p50_us\":"     + J::num(r.tick_latency.p50_ns / 1000.0) +
           ",\"tick_p99_us\":"     + J::num(r.tick_latency.p99_ns / 1000.0) +
           ",\"throughput_eps\":"  + J::num(r.throughput_eps) +
           ",\"equity_curve\":"    + equity +
           ",\"trades\":"          + trades_str + "}";
}

} // namespace hft
