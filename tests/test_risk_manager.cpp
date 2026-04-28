// tests/test_risk_manager.cpp
// Unit tests for the risk management gateway

#include "risk/risk_manager.hpp"
#include "core/types.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

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

static Order make_order(const std::string& sym, Side side, OrderType type,
                        double price, Quantity qty) {
    Order o;
    o.id = 1; o.symbol = sym; o.side = side; o.type = type;
    o.price = to_price(price); o.qty = qty;
    return o;
}

// ─── Basic Approval ──────────────────────────────────────────────────────────

TEST(test_valid_order_approved) {
    RiskLimits limits;
    limits.max_order_qty = 1000;
    limits.max_position = 10000;
    limits.max_notional_usd = 1000000;
    limits.max_orders_per_sec = 100;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 100);
    auto result = rm.check_order(o);

    ASSERT_TRUE(result.approved);
    ASSERT_EQ(rm.orders_checked(), 1);
    ASSERT_EQ(rm.orders_rejected(), 0);
    PASS();
}

// ─── Rejection Tests ─────────────────────────────────────────────────────────

TEST(test_reject_qty_too_large) {
    RiskLimits limits;
    limits.max_order_qty = 100;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 500);
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::MAX_ORDER_QTY);
    PASS();
}

TEST(test_reject_zero_qty) {
    RiskLimits limits;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 0);
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::INVALID_QTY);
    PASS();
}

TEST(test_reject_negative_qty) {
    RiskLimits limits;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, -10);
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::INVALID_QTY);
    PASS();
}

TEST(test_reject_negative_limit_price) {
    RiskLimits limits;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, -5.0, 100);
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::INVALID_PRICE);
    PASS();
}

TEST(test_reject_notional_too_large) {
    RiskLimits limits;
    limits.max_order_qty = 100000;
    limits.max_notional_usd = 1000.0;  // very low limit
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 100);
    // notional = 185 * 100 = 18500 > 1000
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::MAX_NOTIONAL);
    PASS();
}

TEST(test_reject_position_limit) {
    RiskLimits limits;
    limits.max_order_qty = 10000;
    limits.max_position = 100;
    limits.max_notional_usd = 1e9;
    limits.max_orders_per_sec = 10000;
    RiskManager rm(limits);

    // Fill a position of 90
    rm.on_fill("AAPL", Side::BUY, 90, 185.0);

    // Try to buy 20 more → projected = 110 > max 100
    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 20);
    auto result = rm.check_order(o);

    ASSERT_TRUE(!result.approved);
    ASSERT_EQ(result.reason, RiskRejectReason::MAX_POSITION);
    PASS();
}

// ─── Rate Limiter ────────────────────────────────────────────────────────────

TEST(test_rejected_orders_dont_count_toward_rate_limit) {
    // This is the Bug #9 regression test
    RiskLimits limits;
    limits.max_order_qty = 100;
    limits.max_orders_per_sec = 5;
    limits.max_notional_usd = 1e9;
    limits.max_position = 1000000;
    RiskManager rm(limits);

    // Send 10 INVALID orders (qty too large) — these should NOT count
    for (int i = 0; i < 10; ++i) {
        auto bad = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 500);
        rm.check_order(bad);
    }

    // Now send a valid order — it should still be approved
    auto good = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 50);
    auto result = rm.check_order(good);

    ASSERT_TRUE(result.approved);  // rate limit not exhausted by rejects
    PASS();
}

// ─── Position & PnL Tracking ─────────────────────────────────────────────────

TEST(test_position_after_fill) {
    RiskLimits limits;
    RiskManager rm(limits);

    rm.on_fill("AAPL", Side::BUY, 100, 185.0);
    auto pos = rm.get_position("AAPL");

    ASSERT_EQ(pos.net_qty, 100);
    ASSERT_TRUE(std::abs(pos.avg_price - 185.0) < 0.01);
    PASS();
}

TEST(test_realized_pnl) {
    RiskLimits limits;
    RiskManager rm(limits);

    // Buy 100 @ 185
    rm.on_fill("AAPL", Side::BUY, 100, 185.0);
    // Sell 100 @ 190 → realized = (190 - 185) * 100 = 500
    rm.on_fill("AAPL", Side::SELL, 100, 190.0);

    auto pos = rm.get_position("AAPL");
    ASSERT_EQ(pos.net_qty, 0);
    ASSERT_TRUE(std::abs(pos.realized_pnl - 500.0) < 0.01);
    PASS();
}

TEST(test_unrealized_pnl) {
    RiskLimits limits;
    RiskManager rm(limits);

    rm.on_fill("AAPL", Side::BUY, 100, 185.0);
    rm.update_market_price("AAPL", 190.0);

    auto pos = rm.get_position("AAPL");
    // unrealized = (190 - 185) * 100 = 500
    ASSERT_TRUE(std::abs(pos.unrealized_pnl - 500.0) < 0.01);
    PASS();
}

TEST(test_disabled_risk_approves_all) {
    RiskLimits limits;
    limits.enabled = false;
    RiskManager rm(limits);

    auto o = make_order("AAPL", Side::BUY, OrderType::LIMIT, 185.0, 999999);
    auto result = rm.check_order(o);

    ASSERT_TRUE(result.approved);
    PASS();
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== Risk Manager — Unit Tests ===\n\n";

    std::cout << "\n─────────────────────────────────────────────\n";
    std::cout << "  Passed: " << tests_passed << "\n";
    std::cout << "  Failed: " << tests_failed << "\n";
    std::cout << "─────────────────────────────────────────────\n\n";

    return tests_failed > 0 ? 1 : 0;
}
