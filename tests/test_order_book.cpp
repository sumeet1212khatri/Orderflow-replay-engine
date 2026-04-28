// tests/test_order_book.cpp
// Unit tests for the matching engine core
// Build: cmake --build build --target hft_tests && ./build/hft_tests

#include "core/order_book.hpp"
#include "core/types.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>

using namespace hft;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    void name(); \
    struct name##_register { name##_register() { std::cout << "  " << #name << "... "; name(); } } name##_instance; \
    void name()

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "FAIL: " << #a << " == " << (a) << ", expected " << (b) << " at line " << __LINE__ << "\n"; \
        ++tests_failed; return; \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "FAIL: " << #x << " at line " << __LINE__ << "\n"; \
        ++tests_failed; return; \
    } \
} while(0)

#define PASS() do { std::cout << "PASS\n"; ++tests_passed; } while(0)

// ─── Price Conversion Tests ──────────────────────────────────────────────────

TEST(test_to_price_positive) {
    ASSERT_EQ(to_price(185.50), 1855000);
    ASSERT_EQ(to_price(0.0001), 1);
    ASSERT_EQ(to_price(100.0), 1000000);
    PASS();
}

TEST(test_to_price_negative) {
    ASSERT_EQ(to_price(-1.5), -15000);
    ASSERT_EQ(to_price(-0.0001), -1);
    ASSERT_EQ(to_price(-100.0), -1000000);
    PASS();
}

TEST(test_to_price_roundtrip) {
    double original = 185.4567;
    Price p = to_price(original);
    double back = from_price(p);
    ASSERT_TRUE(std::abs(back - original) < 0.00005);
    PASS();
}

// ─── Order Book: Basic Insert ────────────────────────────────────────────────

TEST(test_add_limit_order_no_match) {
    OrderBook book("AAPL");
    Order buy;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;

    auto trades = book.add_order(buy);
    ASSERT_EQ(trades.size(), 0u);
    ASSERT_EQ(book.order_count(), 1u);
    ASSERT_EQ(book.bid_levels(), 1u);
    ASSERT_EQ(book.ask_levels(), 0u);
    ASSERT_EQ(book.best_bid(), to_price(185.0));
    PASS();
}

TEST(test_multiple_levels) {
    OrderBook book("AAPL");
    Order o1, o2, o3;
    o1.id = 1; o1.symbol = "AAPL"; o1.side = Side::BUY;
    o1.type = OrderType::LIMIT; o1.price = to_price(185.0); o1.qty = 100;

    o2.id = 2; o2.symbol = "AAPL"; o2.side = Side::BUY;
    o2.type = OrderType::LIMIT; o2.price = to_price(184.0); o2.qty = 200;

    o3.id = 3; o3.symbol = "AAPL"; o3.side = Side::SELL;
    o3.type = OrderType::LIMIT; o3.price = to_price(186.0); o3.qty = 150;

    book.add_order(o1);
    book.add_order(o2);
    book.add_order(o3);

    ASSERT_EQ(book.bid_levels(), 2u);
    ASSERT_EQ(book.ask_levels(), 1u);
    ASSERT_EQ(book.best_bid(), to_price(185.0));
    ASSERT_EQ(book.best_ask(), to_price(186.0));
    PASS();
}

// ─── Order Book: Matching ────────────────────────────────────────────────────

TEST(test_limit_buy_matches_ask) {
    OrderBook book("AAPL");

    Order sell;
    sell.id = 1; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 100;
    book.add_order(sell);

    Order buy;
    buy.id = 2; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].qty, 100);
    ASSERT_EQ(trades[0].price, to_price(185.0));
    ASSERT_EQ(book.order_count(), 0u);
    PASS();
}

TEST(test_limit_sell_matches_bid) {
    OrderBook book("AAPL");

    Order buy;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;
    book.add_order(buy);

    Order sell;
    sell.id = 2; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 100;
    auto trades = book.add_order(sell);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].qty, 100);
    ASSERT_EQ(book.order_count(), 0u);
    PASS();
}

TEST(test_partial_fill) {
    OrderBook book("AAPL");

    Order sell;
    sell.id = 1; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 200;
    book.add_order(sell);

    Order buy;
    buy.id = 2; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 50;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].qty, 50);
    ASSERT_EQ(book.order_count(), 1u);  // resting sell still has 150
    ASSERT_EQ(book.ask_levels(), 1u);
    PASS();
}

TEST(test_multi_level_sweep) {
    OrderBook book("AAPL");

    Order s1, s2;
    s1.id = 1; s1.symbol = "AAPL"; s1.side = Side::SELL;
    s1.type = OrderType::LIMIT; s1.price = to_price(185.0); s1.qty = 100;

    s2.id = 2; s2.symbol = "AAPL"; s2.side = Side::SELL;
    s2.type = OrderType::LIMIT; s2.price = to_price(186.0); s2.qty = 100;

    book.add_order(s1);
    book.add_order(s2);

    // Aggressive buy sweeps both levels
    Order buy;
    buy.id = 3; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(187.0); buy.qty = 200;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 2u);
    ASSERT_EQ(trades[0].price, to_price(185.0));  // best ask first
    ASSERT_EQ(trades[1].price, to_price(186.0));
    ASSERT_EQ(book.order_count(), 0u);
    ASSERT_EQ(book.ask_levels(), 0u);
    PASS();
}

TEST(test_sell_side_multi_level_sweep) {
    // This specifically tests the sell-side iterator fix (Bug #4)
    OrderBook book("AAPL");

    Order b1, b2, b3;
    b1.id = 1; b1.symbol = "AAPL"; b1.side = Side::BUY;
    b1.type = OrderType::LIMIT; b1.price = to_price(185.0); b1.qty = 100;

    b2.id = 2; b2.symbol = "AAPL"; b2.side = Side::BUY;
    b2.type = OrderType::LIMIT; b2.price = to_price(184.0); b2.qty = 100;

    b3.id = 3; b3.symbol = "AAPL"; b3.side = Side::BUY;
    b3.type = OrderType::LIMIT; b3.price = to_price(183.0); b3.qty = 100;

    book.add_order(b1);
    book.add_order(b2);
    book.add_order(b3);

    // Aggressive sell sweeps all 3 bid levels
    Order sell;
    sell.id = 4; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::MARKET; sell.price = 0; sell.qty = 300;
    auto trades = book.add_order(sell);

    ASSERT_EQ(trades.size(), 3u);
    ASSERT_EQ(trades[0].price, to_price(185.0));  // best bid first
    ASSERT_EQ(trades[1].price, to_price(184.0));
    ASSERT_EQ(trades[2].price, to_price(183.0));
    ASSERT_EQ(book.bid_levels(), 0u);
    ASSERT_EQ(book.order_count(), 0u);
    PASS();
}

// ─── Order Types ─────────────────────────────────────────────────────────────

TEST(test_market_order_no_counterparty) {
    OrderBook book("AAPL");

    Order buy;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::MARKET; buy.price = 0; buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 0u);
    ASSERT_EQ(buy.status, OrderStatus::CANCELLED);
    ASSERT_EQ(book.order_count(), 0u);
    PASS();
}

TEST(test_ioc_partial_fill) {
    OrderBook book("AAPL");

    Order sell;
    sell.id = 1; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 50;
    book.add_order(sell);

    Order buy;
    buy.id = 2; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::IOC; buy.price = to_price(185.0); buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].qty, 50);
    ASSERT_EQ(buy.status, OrderStatus::EXPIRED);  // unfilled remainder expired
    ASSERT_EQ(book.order_count(), 0u);  // IOC not added to book
    PASS();
}

TEST(test_fok_rejected_insufficient_liquidity) {
    OrderBook book("AAPL");

    Order sell;
    sell.id = 1; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 50;
    book.add_order(sell);

    Order buy;
    buy.id = 2; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::FOK; buy.price = to_price(185.0); buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 0u);
    ASSERT_EQ(buy.status, OrderStatus::EXPIRED);
    ASSERT_EQ(book.order_count(), 1u);  // resting sell untouched
    PASS();
}

TEST(test_fok_filled_sufficient_liquidity) {
    OrderBook book("AAPL");

    Order sell;
    sell.id = 1; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.0); sell.qty = 100;
    book.add_order(sell);

    Order buy;
    buy.id = 2; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::FOK; buy.price = to_price(185.0); buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].qty, 100);
    ASSERT_EQ(book.order_count(), 0u);
    PASS();
}

// ─── Cancel & Modify ─────────────────────────────────────────────────────────

TEST(test_cancel_order) {
    OrderBook book("AAPL");

    Order buy;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;
    book.add_order(buy);

    ASSERT_TRUE(book.cancel_order(1));
    ASSERT_EQ(book.order_count(), 0u);
    ASSERT_EQ(book.bid_levels(), 0u);
    ASSERT_EQ(buy.status, OrderStatus::CANCELLED);
    PASS();
}

TEST(test_cancel_nonexistent) {
    OrderBook book("AAPL");
    ASSERT_TRUE(!book.cancel_order(999));
    PASS();
}

TEST(test_modify_order) {
    OrderBook book("AAPL");

    Order buy;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;
    book.add_order(buy);

    ASSERT_TRUE(book.modify_order(1, to_price(186.0), 200));
    ASSERT_EQ(book.best_bid(), to_price(186.0));
    PASS();
}

// ─── Snapshot ────────────────────────────────────────────────────────────────

TEST(test_snapshot_depth) {
    OrderBook book("AAPL");
    for (int i = 1; i <= 20; ++i) {
        Order o;
        o.id = i; o.symbol = "AAPL"; o.side = Side::BUY;
        o.type = OrderType::LIMIT; o.price = to_price(185.0 - i * 0.01); o.qty = 100;
        book.add_order(o);
    }

    auto snap = book.snapshot(5);
    ASSERT_EQ((int)snap.bids.size(), 5);  // only top 5
    ASSERT_TRUE(snap.bids[0].price > snap.bids[1].price);  // sorted descending
    PASS();
}

TEST(test_snapshot_spread) {
    OrderBook book("AAPL");
    Order buy, sell;
    buy.id = 1; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;

    sell.id = 2; sell.symbol = "AAPL"; sell.side = Side::SELL;
    sell.type = OrderType::LIMIT; sell.price = to_price(185.10); sell.qty = 100;

    book.add_order(buy);
    book.add_order(sell);

    auto snap = book.snapshot();
    ASSERT_TRUE(snap.spread() > 0);
    ASSERT_TRUE(std::abs(snap.spread() - 0.10) < 0.001);
    PASS();
}

TEST(test_empty_book_spread) {
    OrderBookSnapshot empty;
    ASSERT_TRUE(empty.spread() == 0.0);
    ASSERT_TRUE(empty.mid() == 0);
    PASS();
}

// ─── FIFO Priority ──────────────────────────────────────────────────────────

TEST(test_fifo_priority) {
    OrderBook book("AAPL");

    // Two sells at same price — first should fill first
    Order s1, s2;
    s1.id = 1; s1.symbol = "AAPL"; s1.side = Side::SELL;
    s1.type = OrderType::LIMIT; s1.price = to_price(185.0); s1.qty = 100;

    s2.id = 2; s2.symbol = "AAPL"; s2.side = Side::SELL;
    s2.type = OrderType::LIMIT; s2.price = to_price(185.0); s2.qty = 100;

    book.add_order(s1);
    book.add_order(s2);

    Order buy;
    buy.id = 3; buy.symbol = "AAPL"; buy.side = Side::BUY;
    buy.type = OrderType::LIMIT; buy.price = to_price(185.0); buy.qty = 100;
    auto trades = book.add_order(buy);

    ASSERT_EQ(trades.size(), 1u);
    ASSERT_EQ(trades[0].sell_order_id, 1u);  // s1 filled first (FIFO)
    ASSERT_EQ(book.order_count(), 1u);       // s2 still resting
    PASS();
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Orderflow Replay Engine — Unit Tests ===\n\n";
    // Tests run automatically via static initialization

    std::cout << "\n─────────────────────────────────────────────\n";
    std::cout << "  Passed: " << tests_passed << "\n";
    std::cout << "  Failed: " << tests_failed << "\n";
    std::cout << "─────────────────────────────────────────────\n\n";

    return tests_failed > 0 ? 1 : 0;
}
