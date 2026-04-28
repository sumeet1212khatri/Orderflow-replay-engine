#include "core/simulator.hpp"
#include "core/types.hpp"
#include "strategy/strategy.hpp"
#include "network/serializer.hpp"

// httplib single-header
#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "httplib.h"

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <cstring>

using namespace hft;

// ── Global state ──────────────────────────────────────────────────────────────
static RiskLimits g_limits;
static std::unique_ptr<ExchangeSimulator> g_sim;
static std::vector<MarketDataTick> g_tick_feed;
static std::atomic<bool> g_feed_running{false};
static std::mutex g_feed_mtx;

static std::deque<std::string> g_recent_trades;   // JSON strings, max 200
static std::deque<std::string> g_recent_orders;
static std::mutex g_log_mtx;
static const int MAX_LOG = 200;

static void log_trade(const Trade& t) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_recent_trades.push_back(trade_to_json(t));
    if ((int)g_recent_trades.size() > MAX_LOG) g_recent_trades.pop_front();
}

static void log_order(const Order& o) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_recent_orders.push_back(order_to_json(o));
    if ((int)g_recent_orders.size() > MAX_LOG) g_recent_orders.pop_front();
}

// ── Simple JSON param parser ──────────────────────────────────────────────────
static std::string json_get(const std::string& body, const std::string& key) {
    // Extremely basic key extraction — good enough for our controlled API
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

static void reset_sim() {
    g_sim = std::make_unique<ExchangeSimulator>(g_limits);
    // Default symbols
    for (auto& sym : {"AAPL", "GOOG", "MSFT", "TSLA", "NVDA"})
        g_sim->add_symbol(sym);
    g_sim->set_trade_callback(log_trade);
    g_sim->set_order_callback(log_order);
}

int main(int argc, char* argv[]) {
    int port = 7860; // HuggingFace default
    if (argc > 1) port = std::atoi(argv[1]);

    g_limits.max_order_qty    = 10000;
    g_limits.max_position     = 500000;
    g_limits.max_notional_usd = 50000000.0;
    g_limits.max_orders_per_sec = 2000;
    reset_sim();

    // Pre-generate tick feed
    {
        const char* syms[] = {"AAPL", "GOOG", "MSFT", "TSLA", "NVDA"};
        double prices[] = {185.0, 140.0, 375.0, 245.0, 485.0};
        for (int i = 0; i < 5; ++i) {
            MarketDataParser::Config cfg;
            cfg.symbol = syms[i]; cfg.start_price = prices[i];
            cfg.ticks  = 2000; cfg.volatility = 0.0008;
            cfg.seed   = 42 + i;
            auto ticks = MarketDataParser::generate_synthetic(cfg);
            for (auto& t : ticks) g_tick_feed.push_back(t);
        }
        // Sort by timestamp
        std::sort(g_tick_feed.begin(), g_tick_feed.end(),
            [](auto& a, auto& b){ return a.timestamp < b.timestamp; });
    }

    httplib::Server svr;

    // CORS
    svr.set_default_headers({
        {"Access-Control-Allow-Origin",  "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"}
    });
    svr.Options(".*", [](auto&, auto& res){ res.status = 204; });

    // ── GET /api/status ──────────────────────────────────────────────────────
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        auto& s = *g_sim;
        std::string body = "{\"status\":\"ok\""
            ",\"total_orders\":" + J::num(s.total_orders()) +
            ",\"total_trades\":" + J::num(s.total_trades()) +
            ",\"total_rejects\":" + J::num(s.total_rejects()) +
            ",\"total_pnl\":" + J::num(s.total_pnl()) +
            ",\"order_p50_us\":" + J::num(s.order_latency().p50_ns / 1000.0) +
            ",\"order_p99_us\":" + J::num(s.order_latency().p99_ns / 1000.0) +
            ",\"tick_p50_us\":"  + J::num(s.tick_latency().p50_ns / 1000.0) +
            ",\"tick_p99_us\":"  + J::num(s.tick_latency().p99_ns / 1000.0) +
            ",\"risk_checked\":" + J::num(s.risk().orders_checked()) +
            ",\"risk_rejected\":" + J::num(s.risk().orders_rejected()) +
            ",\"feed_size\":" + J::num((int64_t)g_tick_feed.size()) +
            "}";
        res.set_content(body, "application/json");
    });

    // ── GET /api/book/:symbol ─────────────────────────────────────────────────
    svr.Get(R"(/api/book/([A-Z]+))", [](const httplib::Request& req, httplib::Response& res) {
        auto sym = req.matches[1].str();
        auto snap = g_sim->get_snapshot(sym);
        if (snap.symbol.empty()) {
            res.status = 404;
            res.set_content("{\"error\":\"symbol not found\"}", "application/json");
            return;
        }
        res.set_content(snapshot_to_json(snap), "application/json");
    });

    // ── GET /api/positions ───────────────────────────────────────────────────
    svr.Get("/api/positions", [](const httplib::Request&, httplib::Response& res) {
        auto pos = g_sim->all_positions();
        std::string body = "[";
        bool first = true;
        for (auto& [sym, p] : pos) {
            if (!first) body += ",";
            body += position_to_json(p);
            first = false;
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    // ── GET /api/trades ───────────────────────────────────────────────────────
    svr.Get("/api/trades", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        std::string body = "[";
        bool first = true;
        for (auto& t : g_recent_trades) {
            if (!first) body += ",";
            body += t; first = false;
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    // ── GET /api/orders ───────────────────────────────────────────────────────
    svr.Get("/api/orders", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        std::string body = "[";
        bool first = true;
        for (auto& o : g_recent_orders) {
            if (!first) body += ",";
            body += o; first = false;
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    // ── POST /api/order ──────────────────────────────────────────────────────
    svr.Post("/api/order", [](const httplib::Request& req, httplib::Response& res) {
        auto& body = req.body;
        Order order;
        order.id      = g_sim->next_order_id();
        order.symbol  = json_get(body, "symbol");
        std::string side_s = json_get(body, "side");
        order.side    = (side_s == "SELL") ? Side::SELL : Side::BUY;
        std::string type_s = json_get(body, "type");
        if      (type_s == "MARKET") order.type = OrderType::MARKET;
        else if (type_s == "IOC")    order.type = OrderType::IOC;
        else if (type_s == "FOK")    order.type = OrderType::FOK;
        else                          order.type = OrderType::LIMIT;
        try {
            order.price = to_price(std::stod(json_get(body, "price")));
            order.qty   = std::stoll(json_get(body, "qty"));
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid price or qty\"}", "application/json");
            return;
        }
        order.client_id = json_get(body, "client_id");

        auto result = g_sim->submit_order(order);
        std::string resp = "{\"approved\":" + J::boolean(result.approved) +
                           ",\"order\":" + order_to_json(order) +
                           ",\"reject_reason\":" + J::str(result.reject_reason) +
                           ",\"trades\":[";
        for (size_t i = 0; i < result.trades.size(); ++i) {
            if (i) resp += ",";
            resp += trade_to_json(result.trades[i]);
        }
        resp += "]}";
        res.set_content(resp, "application/json");
    });

    // ── DELETE /api/order/:id/:symbol ────────────────────────────────────────
    svr.Delete(R"(/api/order/(\d+)/([A-Z]+))", [](const httplib::Request& req, httplib::Response& res) {
        OrderId id = std::stoull(req.matches[1].str());
        std::string sym = req.matches[2].str();
        bool ok = g_sim->cancel_order(sym, id);
        res.set_content("{\"cancelled\":" + J::boolean(ok) + "}", "application/json");
    });

    // ── POST /api/feed/step?n=N ──────────────────────────────────────────────
    // Advance feed by N ticks, return the ticks
    static std::atomic<size_t> g_feed_idx{0};
    svr.Post("/api/feed/step", [&g_feed_idx](const httplib::Request& req, httplib::Response& res) {
        int n = 1;
        if (req.has_param("n")) {
            try { n = std::stoi(req.get_param_value("n")); } catch(...) {}
        }
        n = std::min(n, 100);

        std::string body = "[";
        bool first = true;
        for (int i = 0; i < n; ++i) {
            size_t idx = g_feed_idx.fetch_add(1) % g_tick_feed.size();
            auto& tick = g_tick_feed[idx];
            g_sim->on_tick(tick);
            if (!first) body += ",";
            body += "{\"symbol\":" + J::str(tick.symbol) +
                    ",\"bid\":" + J::num(from_price(tick.bid_price)) +
                    ",\"ask\":" + J::num(from_price(tick.ask_price)) +
                    ",\"last\":" + J::num(from_price(tick.last_price)) +
                    ",\"bid_qty\":" + J::num(tick.bid_qty) +
                    ",\"ask_qty\":" + J::num(tick.ask_qty) +
                    ",\"ts\":" + J::num(tick.timestamp) + "}";
            first = false;
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    // ── POST /api/backtest ───────────────────────────────────────────────────
    svr.Post("/api/backtest", [](const httplib::Request& req, httplib::Response& res) {
        std::string strategy_name = json_get(req.body, "strategy");
        std::string symbol        = json_get(req.body, "symbol");
        int ticks = 5000;
        try { ticks = std::stoi(json_get(req.body, "ticks")); } catch(...) {}
        ticks = std::min(std::max(ticks, 100), 50000);

        double start_price = 150.0;
        try { start_price = std::stod(json_get(req.body, "start_price")); } catch(...) {}

        if (symbol.empty()) symbol = "AAPL";

        // Generate fresh tick set
        MarketDataParser::Config cfg;
        cfg.symbol      = symbol;
        cfg.start_price = start_price;
        cfg.ticks       = ticks;
        cfg.volatility  = 0.001;
        cfg.seed        = 123;
        auto bt_ticks   = MarketDataParser::generate_synthetic(cfg);

        RiskLimits rl;
        rl.max_order_qty    = 10000;
        rl.max_position     = 200000;
        rl.max_notional_usd = 100000000.0;
        rl.max_orders_per_sec = 5000;

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
        res.set_content(backtest_result_to_json(result), "application/json");
    });

    // ── POST /api/risk/limits ────────────────────────────────────────────────
    svr.Post("/api/risk/limits", [](const httplib::Request& req, httplib::Response& res) {
        try {
            auto& b = req.body;
            g_limits.max_order_qty    = std::stoll(json_get(b, "max_order_qty"));
            g_limits.max_position     = std::stoll(json_get(b, "max_position"));
            g_limits.max_notional_usd = std::stod(json_get(b, "max_notional_usd"));
            g_limits.max_orders_per_sec = std::stoi(json_get(b, "max_orders_per_sec"));
            g_sim->risk().set_limits(g_limits);
            res.set_content("{\"ok\":true}", "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"bad limits\"}", "application/json");
        }
    });

    // ── GET /api/risk/limits ──────────────────────────────────────────────────
    svr.Get("/api/risk/limits", [](const httplib::Request&, httplib::Response& res) {
        auto l = g_sim->risk().get_limits();
        res.set_content(
            "{\"max_order_qty\":" + J::num(l.max_order_qty) +
            ",\"max_position\":" + J::num(l.max_position) +
            ",\"max_notional_usd\":" + J::num(l.max_notional_usd, 0) +
            ",\"max_orders_per_sec\":" + J::num((int64_t)l.max_orders_per_sec) +
            ",\"enabled\":" + J::boolean(l.enabled) + "}",
            "application/json");
    });

    // ── POST /api/reset ───────────────────────────────────────────────────────
    svr.Post("/api/reset", [](const httplib::Request&, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            g_recent_trades.clear();
            g_recent_orders.clear();
        }
        reset_sim();
        res.set_content("{\"reset\":true}", "application/json");
    });

    // ── GET /api/symbols ───────────────────────────────────────────────────────
    svr.Get("/api/symbols", [](const httplib::Request&, httplib::Response& res) {
        auto syms = g_sim->symbols();
        std::string body = "[";
        for (size_t i = 0; i < syms.size(); ++i) {
            if (i) body += ",";
            body += J::str(syms[i]);
        }
        body += "]";
        res.set_content(body, "application/json");
    });

    // ── Serve frontend ────────────────────────────────────────────────────────
    svr.set_mount_point("/", "./frontend/dist");
    // Fallback for SPA
    svr.Get("/.*", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("./frontend/dist/index.html");
        if (f.good()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            res.set_content(content, "text/html");
        } else {
            res.status = 404;
        }
    });

    std::cout << "╔══════════════════════════════════════════╗\n";
    std::cout << "║  HFT Exchange Simulator                  ║\n";
    std::cout << "║  Listening on http://0.0.0.0:" << port << "        ║\n";
    std::cout << "╚══════════════════════════════════════════╝\n";

    svr.listen("0.0.0.0", port);
    return 0;
}