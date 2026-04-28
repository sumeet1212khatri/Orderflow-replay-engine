#pragma once
#include "types.hpp"
#include <map>
#include <unordered_map>
#include <list>
#include <functional>
#include <vector>
#include <optional>
#include <algorithm>

namespace hft {

// Price level: all orders at same price, FIFO queue
struct PriceLevel {
    Price price;
    std::list<Order*> orders;
    Quantity total_qty = 0;

    void add(Order* o) {
        orders.push_back(o);
        total_qty += o->remaining();
    }
    bool empty() const { return orders.empty(); }
};

using TradeCallback  = std::function<void(const Trade&)>;
using OrderCallback  = std::function<void(const Order&)>;

class OrderBook {
public:
    explicit OrderBook(std::string symbol);

    // Returns trades generated
    std::vector<Trade> add_order(Order& order);
    bool cancel_order(OrderId id);
    bool modify_order(OrderId id, Price new_price, Quantity new_qty);

    OrderBookSnapshot snapshot(int depth = 10) const;
    std::optional<Order*> find_order(OrderId id);

    void set_trade_callback(TradeCallback cb) { trade_cb_ = std::move(cb); }
    void set_order_callback(OrderCallback cb) { order_cb_ = std::move(cb); }

    size_t order_count()  const { return orders_.size(); }
    size_t bid_levels()   const { return bids_.size(); }
    size_t ask_levels()   const { return asks_.size(); }
    Price  best_bid()     const { return bids_.empty() ? 0 : bids_.rbegin()->first; }
    Price  best_ask()     const { return asks_.empty() ? 0 : asks_.begin()->first; }
    const std::string& symbol() const { return symbol_; }

private:
    std::string symbol_;
    uint64_t    next_trade_id_ = 1;

    // bids: descending (highest first) → map with reverse
    std::map<Price, PriceLevel> bids_;  // sorted asc, iterate rbegin for best
    std::map<Price, PriceLevel> asks_;  // sorted asc, begin = best ask

    std::unordered_map<OrderId, Order*> orders_;

    TradeCallback trade_cb_;
    OrderCallback order_cb_;

    std::vector<Trade> match_limit(Order& incoming);
    std::vector<Trade> match_market(Order& incoming);
    std::vector<Trade> try_match(Order& incoming, bool fok_check = false);
    Trade make_trade(Order& buy, Order& sell, Price px, Quantity qty);
    void  add_to_book(Order& order);
    void  remove_from_book(Order& order);
    void  notify_order(const Order& o);
};

} // namespace hft