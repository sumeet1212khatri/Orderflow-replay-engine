#include "core/order_book.hpp"
#include <stdexcept>
#include <cassert>

namespace hft {

OrderBook::OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

void OrderBook::notify_order(const Order& o) {
    if (order_cb_) order_cb_(o);
}

Trade OrderBook::make_trade(Order& buy, Order& sell, Price px, Quantity qty) {
    Trade t;
    t.trade_id      = next_trade_id_++;
    t.buy_order_id  = buy.id;
    t.sell_order_id = sell.id;
    t.symbol        = symbol_;
    t.price         = px;
    t.qty           = qty;
    t.timestamp     = now_ns();

    buy.filled_qty  += qty;
    sell.filled_qty += qty;
    buy.status  = (buy.remaining()  == 0) ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;
    sell.status = (sell.remaining() == 0) ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;
    return t;
}

void OrderBook::add_to_book(Order& order) {
    auto& book  = (order.side == Side::BUY) ? bids_ : asks_;
    auto& level = book[order.price];
    level.price = order.price;
    level.orders.push_back(&order);
    level.total_qty += order.remaining();
    orders_[order.id] = &order;
}

void OrderBook::remove_from_book(Order& order) {
    auto& book = (order.side == Side::BUY) ? bids_ : asks_;
    auto it = book.find(order.price);
    if (it == book.end()) return;

    auto& level = it->second;
    Quantity removed = 0;
    level.orders.remove_if([&](Order* o) {
        if (o->id == order.id) {
            removed = o->remaining();
            return true;
        }
        return false;
    });
    level.total_qty -= removed;
    if (level.empty()) book.erase(it);
    orders_.erase(order.id);
}

std::vector<Trade> OrderBook::try_match(Order& incoming, bool fok_check) {
    std::vector<Trade> trades;

    if (fok_check) {
        Quantity available = 0;
        if (incoming.side == Side::BUY) {
            for (auto& [px, lvl] : asks_) {
                if (incoming.type != OrderType::MARKET && px > incoming.price) break;
                available += lvl.total_qty;
                if (available >= incoming.qty) break;
            }
        } else {
            for (auto rit = bids_.rbegin(); rit != bids_.rend(); ++rit) {
                if (incoming.type != OrderType::MARKET && rit->first < incoming.price) break;
                available += rit->second.total_qty;
                if (available >= incoming.qty) break;
            }
        }
        if (available < incoming.qty) {
            incoming.status = OrderStatus::EXPIRED;
            notify_order(incoming);
            return trades;
        }
    }

    if (incoming.side == Side::BUY) {
        for (auto it = asks_.begin(); it != asks_.end() && incoming.remaining() > 0; ) {
            auto& [px, level] = *it;
            if (incoming.type == OrderType::LIMIT || incoming.type == OrderType::IOC) {
                if (px > incoming.price) break;
            }
            while (!level.orders.empty() && incoming.remaining() > 0) {
                Order* resting   = level.orders.front();
                Quantity fill_qty = std::min(incoming.remaining(), resting->remaining());
                Price    fill_px  = resting->price;

                auto trade = make_trade(incoming, *resting, fill_px, fill_qty);
                trades.push_back(trade);
                if (trade_cb_) trade_cb_(trade);

                level.total_qty -= fill_qty;
                notify_order(*resting);
                notify_order(incoming);

                if (resting->is_done()) {
                    level.orders.pop_front();
                    orders_.erase(resting->id);
                }
            }
            if (level.empty()) {
                it = asks_.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        // Sell-side: match against bids (highest first = rbegin)
        // We iterate from the back using a reverse approach but erase safely.
        while (!bids_.empty() && incoming.remaining() > 0) {
            auto it = std::prev(bids_.end()); // best bid = last element
            auto& [px, level] = *it;
            if (incoming.type == OrderType::LIMIT || incoming.type == OrderType::IOC) {
                if (px < incoming.price) break;
            }
            while (!level.orders.empty() && incoming.remaining() > 0) {
                Order* resting   = level.orders.front();
                Quantity fill_qty = std::min(incoming.remaining(), resting->remaining());
                Price    fill_px  = resting->price;

                auto trade = make_trade(*resting, incoming, fill_px, fill_qty);
                trades.push_back(trade);
                if (trade_cb_) trade_cb_(trade);

                level.total_qty -= fill_qty;
                notify_order(*resting);
                notify_order(incoming);

                if (resting->is_done()) {
                    level.orders.pop_front();
                    orders_.erase(resting->id);
                }
            }
            if (level.empty()) {
                bids_.erase(it);
            } else {
                break; // Remaining qty at this level, stop matching
            }
        }
    }

    return trades;
}

std::vector<Trade> OrderBook::add_order(Order& order) {
    std::vector<Trade> trades;
    order.status = OrderStatus::NEW;
    notify_order(order);

    switch (order.type) {
        case OrderType::LIMIT: {
            trades = try_match(order);
            if (!order.is_done()) add_to_book(order);
            break;
        }
        case OrderType::MARKET: {
            trades = try_match(order);
            if (order.remaining() > 0) {
                order.status = OrderStatus::CANCELLED;
                notify_order(order);
            }
            break;
        }
        case OrderType::IOC: {
            trades = try_match(order);
            if (order.remaining() > 0 && !order.is_done()) {
                order.status = OrderStatus::EXPIRED;
                notify_order(order);
            }
            break;
        }
        case OrderType::FOK: {
            trades = try_match(order, true);
            break;
        }
    }
    return trades;
}

bool OrderBook::cancel_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order* o = it->second;
    remove_from_book(*o);
    o->status = OrderStatus::CANCELLED;
    notify_order(*o);
    return true;
}

bool OrderBook::modify_order(OrderId id, Price new_price, Quantity new_qty) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    Order* o = it->second;
    if (new_qty <= o->filled_qty) return false;
    remove_from_book(*o);
    o->price = new_price;
    o->qty   = new_qty;
    add_to_book(*o);
    return true;
}

std::optional<Order*> OrderBook::find_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return std::nullopt;
    return it->second;
}

OrderBookSnapshot OrderBook::snapshot(int depth) const {
    OrderBookSnapshot snap;
    snap.symbol    = symbol_;
    snap.timestamp = now_ns();

    int cnt = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && cnt < depth; ++it, ++cnt) {
        L2Level l;
        l.price       = it->first;
        l.qty         = it->second.total_qty;
        l.order_count = (int)it->second.orders.size();
        snap.bids.push_back(l);
    }

    cnt = 0;
    for (auto it = asks_.begin(); it != asks_.end() && cnt < depth; ++it, ++cnt) {
        L2Level l;
        l.price       = it->first;
        l.qty         = it->second.total_qty;
        l.order_count = (int)it->second.orders.size();
        snap.asks.push_back(l);
    }

    return snap;
}

} // namespace hft
