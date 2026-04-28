#pragma once
#include "core/types.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <cstring>

namespace hft {

enum class JournalRecordType : uint8_t {
    ORDER_NEW=1, ORDER_CANCEL=2, ORDER_MODIFY=3, TRADE=4,
    TICK=5, SNAPSHOT=6, SESSION_START=7, SESSION_END=8
};

#pragma pack(push, 1)
struct JournalOrderRecord {
    OrderId id; int64_t price; int64_t qty; int64_t timestamp;
    uint8_t side; uint8_t type; uint8_t status;
    char symbol[16]; char client_id[32];
};
struct JournalTradeRecord {
    uint64_t trade_id; OrderId buy_id; OrderId sell_id;
    int64_t price; int64_t qty; int64_t timestamp; char symbol[16];
};
struct JournalTickRecord {
    int64_t timestamp, bid_price, bid_qty, ask_price, ask_qty, last_price, last_qty;
    char symbol[16];
};
#pragma pack(pop)

class BinaryJournal {
public:
    explicit BinaryJournal(const std::string& path);
    ~BinaryJournal();
    bool open_write(); bool open_read(); void close();
    void write_order(JournalRecordType type, const Order& order);
    void write_trade(const Trade& trade);
    void write_tick(const MarketDataTick& tick);
    void write_session_start(); void write_session_end();
    struct Record { JournalRecordType type; std::vector<uint8_t> payload; };
    std::vector<Record> read_all();
    size_t records_written() const { return records_written_; }
private:
    std::string path_;
    std::ofstream wfile_; std::ifstream rfile_;
    size_t records_written_ = 0;
    uint32_t checksum(const uint8_t* data, size_t len);
    void write_record(JournalRecordType type, const void* data, size_t size);
};

class MarketDataParser {
public:
    struct Config {
        std::string symbol = "AAPL"; double start_price = 150.0;
        double volatility = 0.001; double spread_bps = 5.0; int ticks = 10000;
        int64_t start_ts = 1700000000000000000LL; int64_t tick_interval_ns = 100000;
        uint64_t seed = 42;
    };
    static std::vector<MarketDataTick> generate_synthetic(const Config& cfg);
    static std::vector<MarketDataTick> parse_csv(const std::string& path);
};

} // namespace hft
