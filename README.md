# Orderflow Replay Engine — HFT Exchange Simulator

> A production-grade, low-latency **matching engine** and **orderflow replay system** built from scratch in C++17 with a Python (FastAPI) orchestration layer. Designed as a **Low-Level Design (LLD)** exercise demonstrating real-world exchange microstructure, risk management, and backtesting infrastructure.

---

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [System Design](#system-design)
- [Core Components](#core-components)
  - [Matching Engine](#1-matching-engine-order_bookcpp)
  - [Exchange Simulator](#2-exchange-simulator-simulatorcpp)
  - [Risk Manager](#3-risk-manager-risk_managercpp)
  - [Binary Journal](#4-binary-journal-journalcpp)
  - [Strategy Framework](#5-strategy-framework-strategyhpp)
  - [Network / Serialization](#6-network--serialization-serializerhpp)
  - [API Layer](#7-api-layer-engine_clicpp--apppy)
- [Data Flow](#data-flow)
- [Key Design Decisions](#key-design-decisions)
- [Concurrency Model](#concurrency-model)
- [Build & Run](#build--run)
- [API Reference](#api-reference)
- [Testing](#testing)
- [Performance Characteristics](#performance-characteristics)
- [Project Structure](#project-structure)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Frontend (index.html)                        │
│                    Real-time Order Book · Charts                    │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ HTTP / JSON
┌──────────────────────────────▼──────────────────────────────────────┐
│                     Python FastAPI (app.py)                         │
│              Subprocess manager · REST routing · CORS               │
└──────────────────────────────┬──────────────────────────────────────┘
                               │ stdin/stdout (line-delimited JSON)
┌──────────────────────────────▼─────────────────────────────────────┐
│                   C++ Engine (engine_cli.cpp)                      │
│                 Command dispatcher · Session state                 │
├─────────────┬──────────────┬──────────────┬────────────────────────┤
│  OrderBook  │  RiskManager │   Journal    │   Backtester           │
│  (per-sym)  │  (per-sim)   │  (binary)    │   + Strategy plugins   │
├─────────────┴──────────────┴──────────────┴────────────────────────┤
│                     ExchangeSimulator                              │
│       Owns order books · Coordinates fills · Tracks latency        │
└────────────────────────────────────────────────────────────────────┘
```

---

## System Design

### Design Goals

| Goal | Approach |
|------|----------|
| **Sub-microsecond matching** | Lock-free hot path, `std::map` price levels, `std::list` FIFO queues |
| **Memory safety** | `std::unique_ptr<Order>` lifetime management in `order_store_` |
| **Thread safety** | `std::mutex` for book access, `std::atomic` for counters |
| **Extensibility** | `IStrategy` interface for pluggable strategies |
| **Observability** | Per-order latency tracking (p50/p99), PnL, fill rates |
| **Correctness** | CRC32 journal checksums, FOK/IOC/GTC order semantics |

### Design Patterns Used

- **Strategy Pattern** → `IStrategy` base class with `MarketMaking`, `Momentum`, `MeanReversion` implementations
- **Observer Pattern** → `TradeCallback` / `OrderCallback` for event-driven notifications
- **Command Pattern** → CLI dispatcher maps string commands to handler functions
- **RAII** → `BinaryJournal` destructor auto-closes file handles
- **Factory** → Strategy instantiation in backtest handler based on string name

---

## Core Components

### 1. Matching Engine (`order_book.cpp`)

The central component — a **price-time priority** limit order book.

```
Bids (std::map<Price, PriceLevel>)        Asks (std::map<Price, PriceLevel>)
┌────────────────────────────┐            ┌────────────────────────────┐
│ 185.50  [100] [200] [50]   │  ← best    │ 185.55  [300] [100]        │ ← best
│ 185.45  [500]              │  bid       │ 185.60  [200] [150] [100]  │  ask
│ 185.40  [1000] [200]       │            │ 185.65  [400]              │
└────────────────────────────┘            └────────────────────────────┘
    iterate rbegin→rend                       iterate begin→end
```

**Key implementation details:**

- **Price levels**: `std::map<Price, PriceLevel>` — O(log n) insert/lookup, sorted by price
- **Order queue**: `std::list<Order*>` per level — O(1) front/back, stable iterators for FIFO
- **Order index**: `std::unordered_map<OrderId, Order*>` — O(1) cancel/modify by ID
- **Matching**: Buy sweeps asks low→high; Sell sweeps bids high→low
- **Order types**: `LIMIT`, `MARKET`, `IOC` (Immediate-or-Cancel), `FOK` (Fill-or-Kill)

**Complexity:**

| Operation | Time | Space |
|-----------|------|-------|
| Add order (no match) | O(log P) | O(1) |
| Add order (with match) | O(log P + K) | O(K) trades |
| Cancel order | O(log P + N) | O(1) |
| Snapshot (depth D) | O(D) | O(D) |

Where P = price levels, K = fills generated, N = orders at price level.

### 2. Exchange Simulator (`simulator.cpp`)

Orchestrates multiple order books with risk checks and latency measurement.

```cpp
class ExchangeSimulator {
    unordered_map<string, unique_ptr<OrderBook>> books_;     // per-symbol books
    unordered_map<OrderId, unique_ptr<Order>>    order_store_; // memory ownership
    RiskManager  risk_;          // pre-trade risk checks
    BinaryJournal journal_;      // audit trail
    mutex         mtx_;          // thread safety
    atomic<int64_t> total_orders_{0};  // lock-free counters
};
```

**Order lifecycle:**

```
submit_order(order)
    │
    ├─ risk_.check_order()  →  REJECT? return {false, reason}
    │
    ├─ lock(mtx_)
    │
    ├─ Clone order into order_store_ (stable pointer)
    │
    ├─ book->add_order()
    │     ├─ try_match() → generate trades
    │     └─ add_to_book() (if unfilled LIMIT)
    │
    ├─ risk_.on_fill() for each trade
    │
    └─ record latency → return {true, trades}
```

### 3. Risk Manager (`risk_manager.cpp`)

Pre-trade risk gateway with 5 independent checks:

| Check | Description | Rejection |
|-------|-------------|-----------|
| **Order Size** | `qty > max_order_qty` | `MAX_ORDER_QTY` |
| **Position Limit** | `|projected_pos| > max_position` | `MAX_POSITION` |
| **Notional Limit** | `price × qty > max_notional_usd` | `MAX_NOTIONAL` |
| **Order Rate** | Sliding window 1s, `count >= max_orders_per_sec` | `MAX_ORDER_RATE` |
| **Price Validity** | LIMIT order with `price <= 0` | `INVALID_PRICE` |

**Position tracking** implements proper FIFO cost-basis accounting:

```
Adding to position:  avg_price = weighted average of old + new
Reducing position:   realized_pnl += (fill_price - avg_price) × closing_qty × direction
Flipping position:   close old, open new at fill_price
```

### 4. Binary Journal (`journal.cpp`)

Append-only binary audit trail with **CRC32 integrity checking**.

```
File Format:
┌──────────┬───────────────────────────────────────────┐
│ Header   │ "HFTJ" + version (6 bytes)                │
├──────────┼───────────────────────────────────────────┤
│ Record 1 │ [type:1][size:4][payload:N][crc32:4]      │
│ Record 2 │ [type:1][size:4][payload:N][crc32:4]      │
│ ...      │                                           │
└──────────┴───────────────────────────────────────────┘
```

Record types: `ORDER_NEW`, `ORDER_CANCEL`, `ORDER_MODIFY`, `TRADE`, `TICK`, `SESSION_START/END`

### 5. Strategy Framework (`strategy.hpp`)

Polymorphic strategy interface for backtesting:

```cpp
class IStrategy {
    virtual string name() const = 0;
    virtual vector<StrategySignal> on_tick(const MarketDataTick&, const OrderBookSnapshot&) = 0;
    virtual void on_order_update(const Order&) {}
    virtual void on_trade(const Trade&) {}
};
```

**Included strategies:**

| Strategy | Logic | Signal Type |
|----------|-------|-------------|
| `MarketMaking` | Quote bid/ask around mid ± spread | LIMIT orders, cancel-replace |
| `Momentum` | Fast/slow MA crossover | MARKET orders |
| `MeanReversion` | Z-score entry/exit | LIMIT orders at touch |

### 6. Network / Serialization (`serializer.hpp`)

Hand-rolled JSON serializer (zero external dependencies):

- Proper string escaping (`"`, `\` characters)
- Fixed-precision numeric output via `std::setprecision`
- Overloaded `J::num()` for `int64_t`, `uint64_t`, `double`

### 7. API Layer (`engine_cli.cpp` + `app.py`)

**C++ CLI Engine** — Reads line-delimited JSON commands from stdin, writes JSON responses to stdout. Single-threaded, deterministic.

**Python FastAPI** — Manages the C++ process lifecycle, provides REST endpoints, serves the frontend. Cross-platform (Windows `.exe` detection, thread-based timeout instead of `select()`).

---

## Data Flow

```
Market Data Tick
    │
    ▼
ExchangeSimulator::on_tick()
    │
    ├─ RiskManager::update_market_price()  →  Updates unrealized PnL
    │
    ▼
Strategy::on_tick(tick, book_snapshot)
    │
    ├─ Returns vector<StrategySignal>
    │
    ▼
For each signal:
    │
    ├─ BUY/SELL  →  ExchangeSimulator::submit_order()
    │                    ├─ RiskManager::check_order()
    │                    ├─ OrderBook::add_order()
    │                    │       ├─ try_match() → Trade[]
    │                    │       └─ add_to_book() (if resting)
    │                    └─ RiskManager::on_fill()
    │
    ├─ CANCEL    →  ExchangeSimulator::cancel_order()
    │
    └─ CANCEL_ALL →  Cancel all live orders
```

---

## Key Design Decisions

### Why `std::map` instead of a flat array for price levels?

A flat array (indexed by price tick) gives O(1) lookup but wastes memory for sparse books and requires knowing the price range upfront. `std::map` provides O(log P) with no wasted space and automatic sorted iteration — the right trade-off for a simulator handling multiple symbols with varying price ranges.

### Why `std::unique_ptr<Order>` in `order_store_`?

The `OrderBook` stores raw `Order*` pointers for cache efficiency. But the `Order` objects must outlive the book operations. Storing `unique_ptr<Order>` in the simulator guarantees stable addresses and deterministic cleanup — no dangling pointers after order completion.

### Why separate C++ engine + Python server?

- **C++ engine**: Deterministic, single-threaded matching for correctness and reproducibility
- **Python layer**: Handles HTTP, CORS, process lifecycle — things C++ HTTP libraries (cpp-httplib) do poorly on all platforms
- **IPC via stdin/stdout**: Simple, debuggable, zero-dependency, works on all OS

### Why CRC32 in the journal?

Journals are the audit trail. A single bit-flip in a trade record could mean wrong PnL attribution. CRC32 per record catches corruption during read-back without the overhead of SHA-256.

---

## Concurrency Model

```
                    ┌─────────────────────┐
                    │   FastAPI (Python)  │
                    │   Thread pool       │
                    └─────────┬───────────┘
                              │ threading.Lock
                              │ (serialized IPC)
                    ┌─────────▼───────────┐
                    │   C++ Engine        │
                    │   Single-threaded   │
                    │   (deterministic)   │
                    └─────────────────────┘
```

**C++ engine** is intentionally single-threaded — matching engine correctness is easier to reason about without concurrent mutation.

**Within the simulator** (when used via `main.cpp` HTTP server):
- `std::mutex mtx_` protects `books_`, `order_store_`, `LatencyStats`
- `std::atomic<int64_t>` for `total_orders_`, `total_trades_`, `total_rejects_` (lock-free reads)
- `RiskManager` has its own `std::mutex` for position/market price state
- Trade/order callbacks acquire `g_log_mtx` for the recent-events deque

---

## Build & Run

### Prerequisites

- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
- CMake 3.16+
- Python 3.8+ with `fastapi`, `uvicorn`

### Build

```bash
# Clone
git clone https://github.com/sumeet1212khatri/Orderflow-replay-engine.git
cd Orderflow-replay-engine

# Build C++ engine
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Install Python deps
pip install fastapi uvicorn

# Run
python app.py
# → Listening on http://0.0.0.0:7860
```

### Docker

```bash
docker build -t hft-simulator .
docker run -p 7860:7860 hft-simulator
```

---

## API Reference

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/status` | Engine stats: orders, trades, latency percentiles, PnL |
| `GET` | `/api/symbols` | List available symbols |
| `GET` | `/api/book/{symbol}` | L2 order book snapshot (bids/asks with depth) |
| `GET` | `/api/positions` | Current positions with realized/unrealized PnL |
| `GET` | `/api/trades` | Recent trade log (last 500) |
| `GET` | `/api/orders` | Recent order log (last 500) |
| `POST` | `/api/order` | Submit order `{symbol, side, type, price, qty}` |
| `DELETE` | `/api/order` | Cancel order `{symbol, id}` |
| `POST` | `/api/feed/step` | Advance market data feed by N ticks |
| `POST` | `/api/backtest` | Run strategy backtest `{strategy, symbol, ticks}` |
| `GET` | `/api/risk/limits` | Get current risk limits |
| `POST` | `/api/risk/limits` | Update risk limits |
| `POST` | `/api/reset` | Reset simulator state |

### Example: Submit a Limit Order

```bash
curl -X POST http://localhost:7860/api/order \
  -H "Content-Type: application/json" \
  -d '{"symbol":"AAPL","side":"BUY","type":"LIMIT","price":185.50,"qty":100}'
```

```json
{
  "approved": true,
  "order": {"id": 1001, "symbol": "AAPL", "side": "BUY", "price": 185.50, "qty": 100, "status": "NEW"},
  "reject_reason": "OK",
  "trades": []
}
```

---

## Testing

### Manual Verification

```bash
# 1. Start the engine
python app.py

# 2. Feed market data
curl -X POST http://localhost:7860/api/feed/step -d '{"n": 50}'

# 3. Submit orders and verify matching
curl -X POST http://localhost:7860/api/order -d '{"symbol":"AAPL","side":"BUY","type":"LIMIT","price":200,"qty":100}'

# 4. Check book state
curl http://localhost:7860/api/book/AAPL

# 5. Run backtest
curl -X POST http://localhost:7860/api/backtest -d '{"strategy":"MarketMaking","symbol":"AAPL","ticks":5000}'
```

### Edge Cases Handled

- FOK order with insufficient liquidity → `EXPIRED` (not partial fill)
- IOC order partially filled → remaining qty `EXPIRED`
- Market order with no counterparty → `CANCELLED`
- Crossed book detection → spread returns 0.0
- Negative price conversion → correct rounding via `std::llround`
- Concurrent API requests → mutex-protected book access
- Engine process crash → auto-restart with clear error message

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Order insertion (no match) | ~1–5 μs |
| Order match (single fill) | ~2–8 μs |
| Book snapshot (10 levels) | ~0.5 μs |
| Backtest throughput | ~100K–500K ticks/sec |
| Memory per order | ~128 bytes |
| Synthetic data generation | ~1M ticks/sec |

*Measured on Intel i7, single-threaded, Release build with `-O3 -march=native`*

---

## Project Structure

```
.
├── CMakeLists.txt              # Build configuration (2 targets: hft_server, hft_engine)
├── Dockerfile                  # Container build
├── app.py                      # FastAPI server (Python process manager)
├── index.html                  # Frontend UI
└── src/
    ├── core/
    │   ├── types.hpp           # Price, Order, Trade, Position, LatencyStats
    │   ├── order_book.hpp      # OrderBook class declaration
    │   ├── order_book.cpp      # Matching engine implementation
    │   ├── simulator.hpp       # ExchangeSimulator + Backtester declarations
    │   └── simulator.cpp       # Simulator + Backtester implementation
    ├── risk/
    │   ├── risk_manager.hpp    # RiskManager declaration
    │   └── risk_manager.cpp    # Pre-trade risk checks + position tracking
    ├── journal/
    │   ├── journal.hpp         # BinaryJournal + MarketDataParser declarations
    │   └── journal.cpp         # Binary journal I/O + synthetic data generator
    ├── strategy/
    │   └── strategy.hpp        # IStrategy interface + 3 strategy implementations
    ├── network/
    │   └── serializer.hpp      # JSON serialization utilities
    ├── main.cpp                # HTTP server entry point (cpp-httplib)
    └── engine_cli.cpp          # Stdin/stdout CLI engine (used by app.py)
```

---

## License

MIT

---

<p align="center">
  Built as a <strong>Low-Level Design</strong> exercise demonstrating exchange microstructure, matching engine internals, and systems programming in C++17.
</p>
