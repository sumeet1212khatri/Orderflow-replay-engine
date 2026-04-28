#include "core/simulator.hpp"
#include "core/types.hpp"
#include "strategy/strategy.hpp"
#include "network/serializer.hpp"

#include <iostream>
#include <string>
#include <sstream>
#include <unordered_map>
#include <memory>
#include <unordered_set>
#include <fstream>

using namespace hft;

static std::string jget(const std::string& body, const std::string& key) {
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = body.find(":", pos);
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size()) return "";
    if (body[pos] == '"') {
        ++pos;
        size_t end = body.find('"', pos);
        return body.substr(pos, end - pos);
    }
    size_t end = body.find_first_of(",}\n", pos);
    return body.substr(pos, end - pos);
}

static RiskLimits g_limits;
static std::unique_ptr<ExchangeSimulator> g_sim;
static std::vector<MarketDataTick> g_tick_feed;
static size_t g_feed_idx = 0;
static std::deque<std::string> g_trade_log;
static std::deque<std::string> g_order_log;

static void init_sim() {
    g_sim = std::make_unique<ExchangeSimulator>(g_limits);
    for (auto& sym : {"AAPL", "GOOG", "MSFT", "TSLA", "NVDA"})
        g_sim->add_symbol(sym);
    g_sim->set_trade_callback([](const Trade& t) {
        g_trade_log.push_back(trade_to_json(t));
        if ((int)g_trade_log.size() > 500) g_trade_log.pop_front();
    });
    g_sim->set_order_callback([](const Order& o) {
        g_order_log.push_back(order_to_json(o));
        if ((int)g_order_log.size() > 500) g_order_log.pop_front();
    });
}

static void init_feed() {
    const char* syms[]  = {"AAPL", "GOOG", "MSFT", "TSLA", "NVDA"};
    double prices[]     = {185.0, 140.0, 375.0, 245.0, 485.0};
    g_tick_feed.clear();
    for (int i = 0; i < 5; ++i) {
        MarketDataParser::Config cfg;
        cfg.symbol     = syms[i];
        cfg.start_price = prices[i];
        cfg.ticks      = 3000;
        cfg.volatility = 0.0008;
        cfg.spread_bps = 3.0;
        cfg.seed       = 42 + i;
        auto ticks = MarketDataParser::generate_synthetic(cfg);
        for (auto& t : ticks) g_tick_feed.push_back(t);
    }
    std::sort(g_tick_feed.begin(), g_tick_feed.end(),
        [](const MarketDataTick& a, const MarketDataTick& b) {
            return a.timestamp < b.timestamp;
        });
    g_feed_idx = 0;
}

static std::string handle(const std::string& cmd, const std::string& payload) {

    if (cmd == "status") {
        g_sim->order_latency().compute_percentiles();
        g_sim->tick_latency().compute_percentiles();
        return "{\"total_orders\":"   + J::num(g_sim->total_orders()) +
               ",\"total_trades\":"   + J::num(g_sim->total_trades()) +
               ",\"total_rejects\":"  + J::num(g_sim->total_rejects()) +
               ",\"total_pnl\":"      + J::num(g_sim->total_pnl()) +
               ",\"order_p50_us\":"   + J::num(g_sim->order_latency().p50_ns / 1000.0) +
               ",\"order_p99_us\":"   + J::num(g_sim->order_latency().p99_ns / 1000.0) +
               ",\"tick_p50_us\":"    + J::num(g_sim->tick_latency().p50_ns  / 1000.0) +
               ",\"tick_p99_us\":"    + J::num(g_sim->tick_latency().p99_ns  / 1000.0) +
               ",\"risk_checked\":"   + J::num(g_sim->risk().orders_checked()) +
               ",\"risk_rejected\":"  + J::num(g_sim->risk().orders_rejected()) +
               ",\"feed_size\":"      + J::num((int64_t)g_tick_feed.size()) +
               ",\"feed_idx\":"       + J::num((int64_t)g_feed_idx) + "}";
    }

    if (cmd == "reset") {
        g_trade_log.clear();
        g_order_log.clear();
        init_sim();
        init_feed();
        return "{\"ok\":true}";
    }

    if (cmd == "book") {
        std::string sym = jget(payload, "symbol");
        if (sym.empty()) sym = "AAPL";
        auto snap = g_sim->get_snapshot(sym);
        return snapshot_to_json(snap);
    }

    if (cmd == "positions") {
        auto pos = g_sim->all_positions();
        std::string r = "[";
        bool first = true;
        for (auto& [sym, p] : pos) {
            if (!first) r += ",";
            r += position_to_json(p);
            first = false;
        }
        return r + "]";
    }

    if (cmd == "trades") {
        std::string r = "[";
        bool first = true;
        for (auto& t : g_trade_log) {
            if (!first) r += ",";
            r += t;
            first = false;
        }
        return r + "]";
    }

    if (cmd == "orders") {
        std::string r = "[";
        bool first = true;
        for (auto& o : g_order_log) {
            if (!first) r += ",";
            r += o;
            first = false;
        }
        return r + "]";
    }

    if (cmd == "order") {
        Order order;
        order.id      = g_sim->next_order_id();
        order.symbol  = jget(payload, "symbol");
        if (order.symbol.empty()) order.symbol = "AAPL";
        std::string side_s = jget(payload, "side");
        order.side = (side_s == "SELL") ? Side::SELL : Side::BUY;
        std::string type_s = jget(payload, "type");
        if      (type_s == "MARKET") order.type = OrderType::MARKET;
        else if (type_s == "IOC")    order.type = OrderType::IOC;
        else if (type_s == "FOK")    order.type = OrderType::FOK;
        else                          order.type = OrderType::LIMIT;
        try {
            order.price = to_price(std::stod(jget(payload, "price")));
            order.qty   = std::stoll(jget(payload, "qty"));
        } catch (...) {
            return "{\"error\":\"invalid price or qty\"}";
        }
        order.client_id = jget(payload, "client_id");

        double mkt_px = 0;
        try { mkt_px = std::stod(jget(payload, "market_price")); } catch (...) {}

        auto result = g_sim->submit_order(order, mkt_px);
        std::string r = "{\"approved\":" + J::boolean(result.approved) +
                        ",\"order\":"        + order_to_json(order) +
                        ",\"reject_reason\":" + J::str(result.reject_reason) +
                        ",\"trades\":[";
        for (size_t i = 0; i < result.trades.size(); ++i) {
            if (i) r += ",";
            r += trade_to_json(result.trades[i]);
        }
        return r + "]}";
    }

    if (cmd == "cancel") {
        OrderId id  = 0;
        std::string sym = jget(payload, "symbol");
        try { id = std::stoull(jget(payload, "id")); } catch (...) {}
        bool ok = g_sim->cancel_order(sym, id);
        return "{\"cancelled\":" + J::boolean(ok) + "}";
    }

    if (cmd == "feed_step") {
        int n = 10;
        std::string n_str = jget(payload, "n");
        if (!n_str.empty()) {
            try { n = std::stoi(n_str); } catch (...) {}
        }
        n = std::max(1, std::min(n, 200));

        std::string r = "[";
        bool first = true;
        for (int i = 0; i < n; ++i) {
            if (g_feed_idx >= g_tick_feed.size()) g_feed_idx = 0;
            auto& tick = g_tick_feed[g_feed_idx++];
            g_sim->on_tick(tick);
            if (!first) r += ",";
            r += "{\"symbol\":"  + J::str(tick.symbol) +
                 ",\"bid\":"     + J::num(from_price(tick.bid_price)) +
                 ",\"ask\":"     + J::num(from_price(tick.ask_price)) +
                 ",\"last\":"    + J::num(from_price(tick.last_price)) +
                 ",\"bid_qty\":" + J::num(tick.bid_qty) +
                 ",\"ask_qty\":" + J::num(tick.ask_qty) +
                 ",\"ts\":"      + J::num(tick.timestamp) + "}";
            first = false;
        }
        return r + "]";
    }

    if (cmd == "backtest") {
        std::string strategy_name = jget(payload, "strategy");
        std::string symbol        = jget(payload, "symbol");
        if (symbol.empty()) symbol = "AAPL";
        int ticks = 5000;
        double start_price = 150.0;
        try { ticks       = std::stoi(jget(payload, "ticks")); }       catch (...) {}
        try { start_price = std::stod(jget(payload, "start_price")); } catch (...) {}
        ticks = std::min(std::max(ticks, 200), 50000);

        MarketDataParser::Config cfg;
        cfg.symbol      = symbol;
        cfg.start_price = start_price;
        cfg.ticks       = ticks;
        cfg.volatility  = 0.001;
        cfg.seed        = 777;
        auto bt_ticks   = MarketDataParser::generate_synthetic(cfg);

        RiskLimits rl;
        rl.max_order_qty      = 10000;
        rl.max_position       = 500000;
        rl.max_notional_usd   = 1e9;
        rl.max_orders_per_sec = 10000;

        Backtester bt(rl);
        bt.add_symbol(symbol);

        std::unique_ptr<IStrategy> strat;
        if (strategy_name == "Momentum")
            strat = std::make_unique<MomentumStrategy>();
        else if (strategy_name == "MeanReversion")
            strat = std::make_unique<MeanReversionStrategy>();
        else
            strat = std::make_unique<MarketMakingStrategy>();
        bt.set_strategy(std::move(strat));

        auto result = bt.run(bt_ticks, false);
        return backtest_result_to_json(result);
    }

    if (cmd == "risk_limits") {
        try {
            g_limits.max_order_qty      = std::stoll(jget(payload, "max_order_qty"));
            g_limits.max_position       = std::stoll(jget(payload, "max_position"));
            g_limits.max_notional_usd   = std::stod(jget(payload, "max_notional_usd"));
            g_limits.max_orders_per_sec = std::stoi(jget(payload, "max_orders_per_sec"));
            g_sim->risk().set_limits(g_limits);
            return "{\"ok\":true}";
        } catch (...) {
            return "{\"error\":\"bad limits\"}";
        }
    }

    if (cmd == "get_limits") {
        auto l = g_sim->risk().get_limits();
        return "{\"max_order_qty\":"     + J::num(l.max_order_qty) +
               ",\"max_position\":"      + J::num(l.max_position) +
               ",\"max_notional_usd\":"  + J::num(l.max_notional_usd, 0) +
               ",\"max_orders_per_sec\":" + J::num((int64_t)l.max_orders_per_sec) +
               ",\"enabled\":"           + J::boolean(l.enabled) + "}";
    }

    if (cmd == "symbols") {
        auto syms = g_sim->symbols();
        std::string r = "[";
        for (size_t i = 0; i < syms.size(); ++i) {
            if (i) r += ",";
            r += J::str(syms[i]);
        }
        return r + "]";
    }

    return "{\"error\":" + J::str("unknown command: " + cmd) + "}";
}

int main() {
    g_limits.max_order_qty      = 10000;
    g_limits.max_position       = 500000;
    g_limits.max_notional_usd   = 50000000.0;
    g_limits.max_orders_per_sec = 2000;

    init_sim();
    init_feed();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        size_t sp          = line.find(' ');
        std::string cmd    = (sp == std::string::npos) ? line : line.substr(0, sp);
        std::string payload = (sp == std::string::npos) ? "{}" : line.substr(sp + 1);
        try {
            std::cout << handle(cmd, payload) << "\n";
            std::cout.flush();
        } catch (const std::exception& e) {
            std::cout << "{\"error\":" << J::str(e.what()) << "}\n";
            std::cout.flush();
        }
    }
    return 0;
}
