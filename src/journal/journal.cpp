#include "journal/journal.hpp"
#include <cmath>
#include <random>
#include <stdexcept>
#include <sstream>
#include <fstream>

namespace hft {

BinaryJournal::BinaryJournal(const std::string& path) : path_(path) {}

BinaryJournal::~BinaryJournal() { close(); }

bool BinaryJournal::open_write() {
    wfile_.open(path_, std::ios::binary | std::ios::trunc);
    if (!wfile_.good()) return false;
    // Write magic header
    const char magic[] = "HFTJ\x01\x00";
    wfile_.write(magic, 6);
    return true;
}

bool BinaryJournal::open_read() {
    rfile_.open(path_, std::ios::binary);
    if (!rfile_.good()) return false;
    char magic[6];
    rfile_.read(magic, 6);
    return true;
}

void BinaryJournal::close() {
    if (wfile_.is_open()) wfile_.close();
    if (rfile_.is_open()) rfile_.close();
}

uint32_t BinaryJournal::checksum(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return crc ^ 0xFFFFFFFF;
}

void BinaryJournal::write_record(JournalRecordType type, const void* data, size_t size) {
    if (!wfile_.is_open()) return;
    uint8_t  rtype = (uint8_t)type;
    uint32_t psz   = (uint32_t)size;
    uint32_t crc   = checksum((const uint8_t*)data, size);

    wfile_.write((char*)&rtype, 1);
    wfile_.write((char*)&psz,   4);
    wfile_.write((char*)data,   size);
    wfile_.write((char*)&crc,   4);
    ++records_written_;
}

void BinaryJournal::write_order(JournalRecordType type, const Order& order) {
    JournalOrderRecord rec{};
    rec.id        = order.id;
    rec.price     = order.price;
    rec.qty       = order.qty;
    rec.timestamp = order.timestamp;
    rec.side      = (uint8_t)order.side;
    rec.type      = (uint8_t)order.type;
    rec.status    = (uint8_t)order.status;
    strncpy(rec.symbol,    order.symbol.c_str(),    15);
    strncpy(rec.client_id, order.client_id.c_str(), 31);
    write_record(type, &rec, sizeof(rec));
}

void BinaryJournal::write_trade(const Trade& trade) {
    JournalTradeRecord rec{};
    rec.trade_id  = trade.trade_id;
    rec.buy_id    = trade.buy_order_id;
    rec.sell_id   = trade.sell_order_id;
    rec.price     = trade.price;
    rec.qty       = trade.qty;
    rec.timestamp = trade.timestamp;
    strncpy(rec.symbol, trade.symbol.c_str(), 15);
    write_record(JournalRecordType::TRADE, &rec, sizeof(rec));
}

void BinaryJournal::write_tick(const MarketDataTick& tick) {
    JournalTickRecord rec{};
    rec.timestamp  = tick.timestamp;
    rec.bid_price  = tick.bid_price;
    rec.bid_qty    = tick.bid_qty;
    rec.ask_price  = tick.ask_price;
    rec.ask_qty    = tick.ask_qty;
    rec.last_price = tick.last_price;
    rec.last_qty   = tick.last_qty;
    strncpy(rec.symbol, tick.symbol.c_str(), 15);
    write_record(JournalRecordType::TICK, &rec, sizeof(rec));
}

void BinaryJournal::write_session_start() {
    int64_t ts = now_ns();
    write_record(JournalRecordType::SESSION_START, &ts, sizeof(ts));
}

void BinaryJournal::write_session_end() {
    int64_t ts = now_ns();
    write_record(JournalRecordType::SESSION_END, &ts, sizeof(ts));
}

std::vector<BinaryJournal::Record> BinaryJournal::read_all() {
    std::vector<Record> records;
    if (!rfile_.is_open()) return records;

    while (rfile_.good() && !rfile_.eof()) {
        uint8_t rtype;
        rfile_.read((char*)&rtype, 1);
        if (rfile_.eof()) break;

        uint32_t psz;
        rfile_.read((char*)&psz, 4);
        if (rfile_.fail() || psz > 1<<20) break;

        Record rec;
        rec.type = (JournalRecordType)rtype;
        rec.payload.resize(psz);
        rfile_.read((char*)rec.payload.data(), psz);

        uint32_t crc_stored;
        rfile_.read((char*)&crc_stored, 4);

        // Verify
        uint32_t crc_calc = checksum(rec.payload.data(), psz);
        if (crc_calc != crc_stored) break; // corruption

        records.push_back(std::move(rec));
    }
    return records;
}

// ─── Market Data Generator ────────────────────────────────────────────────────

std::vector<MarketDataTick> MarketDataParser::generate_synthetic(const Config& cfg) {
    std::mt19937_64 rng(cfg.seed);
    std::normal_distribution<double> noise(0.0, cfg.volatility);
    std::uniform_int_distribution<int> qty_dist(100, 5000);

    std::vector<MarketDataTick> ticks;
    ticks.reserve(cfg.ticks);

    double price = cfg.start_price;
    double spread = cfg.start_price * cfg.spread_bps / 10000.0;

    for (int i = 0; i < cfg.ticks; ++i) {
        // Geometric brownian motion tick
        price *= (1.0 + noise(rng));
        if (price <= 0) price = 0.01;

        spread = price * cfg.spread_bps / 10000.0;

        MarketDataTick tick{};
        tick.timestamp  = cfg.start_ts + (int64_t)i * cfg.tick_interval_ns;
        tick.symbol     = cfg.symbol;
        tick.bid_price  = to_price(price - spread / 2.0);
        tick.ask_price  = to_price(price + spread / 2.0);
        tick.bid_qty    = qty_dist(rng);
        tick.ask_qty    = qty_dist(rng);
        tick.last_price = to_price(price);
        tick.last_qty   = qty_dist(rng) / 2;
        ticks.push_back(tick);
    }
    return ticks;
}

std::vector<MarketDataTick> MarketDataParser::parse_csv(const std::string& path) {
    std::vector<MarketDataTick> ticks;
    std::ifstream f(path);
    if (!f.good()) return ticks;

    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        std::vector<std::string> cols;
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        if (cols.size() < 7) continue;

        MarketDataTick tick{};
        try {
            tick.timestamp  = std::stoll(cols[0]);
            tick.symbol     = cols[1];
            tick.bid_price  = to_price(std::stod(cols[2]));
            tick.bid_qty    = std::stoll(cols[3]);
            tick.ask_price  = to_price(std::stod(cols[4]));
            tick.ask_qty    = std::stoll(cols[5]);
            tick.last_price = to_price(std::stod(cols[6]));
            tick.last_qty   = cols.size() > 7 ? std::stoll(cols[7]) : 0;
            ticks.push_back(tick);
        } catch (...) { continue; }
    }
    return ticks;
}

} // namespace hft
