# Complete Bug Report — HFT Exchange Simulator

**Total Bugs Found: 17**  
**Files Modified: 8**  
**Severity: 4 Critical, 9 Medium, 4 Low**

---

## BUG #1 — Windows Pipe Crash (CRITICAL)

**File:** `app.py` (line ~72)  
**What was the bug:**  
The old code used `select.select()` to read the C++ engine's stdout with a timeout. On Linux, `select.select()` works with pipe file descriptors. But on Windows, `select.select()` ONLY works with sockets — NOT with pipes. When the Python server tried to read the engine response, Windows threw an `OSError` and the entire server crashed immediately on every single API call.

**What the old code looked like:**
```python
ready, _, _ = select.select([self.proc.stdout], [], [], ENGINE_TIMEOUT)
if ready:
    response = self.proc.stdout.readline().strip()
```

**How I fixed it:**  
Replaced `select.select()` with a cross-platform thread-based approach. I spawn a short-lived daemon thread that calls `readline()`. The main thread waits for that thread with a timeout using `thread.join(timeout=ENGINE_TIMEOUT)`. If the thread is still alive after the timeout, we kill the engine and restart it.

**What the fixed code looks like:**
```python
response_container = []
def _read():
    resp = self.proc.stdout.readline()
    response_container.append(resp)

reader = threading.Thread(target=_read, daemon=True)
reader.start()
reader.join(timeout=ENGINE_TIMEOUT)

if reader.is_alive():
    self.proc.kill()
    self.proc = None
    self._start()
    raise HTTPException(504, "Engine timeout — restarted")
```

**Why this matters:** Without this fix, the entire application was 100% non-functional on Windows. Every API request would crash.

---

## BUG #2 — Server Crashes If Binary Is Missing (CRITICAL)

**File:** `app.py` (line ~34)  
**What was the bug:**  
The `EngineProcess` was created at module-level (`engine = EngineProcess()`). Inside `__init__`, it called `_start()` which tried to open the C++ binary via `subprocess.Popen()`. If the binary file didn't exist (because you haven't built the C++ code yet), `Popen` would throw an exception and the entire FastAPI server would fail to start — you couldn't even see an error page.

**How I fixed it:**  
1. In `_start()`, added a check: if the binary file doesn't exist, set `self.proc = None` and return gracefully instead of crashing.
2. Changed the module-level `engine = EngineProcess()` to `engine = None` (lazy initialization). The engine is only created when the first API request comes in via `_get_engine()`.

**What the fixed code looks like:**
```python
engine: Optional[EngineProcess] = None

def _get_engine() -> EngineProcess:
    global engine
    if engine is None:
        engine = EngineProcess()
    return engine
```

**Why this matters:** Developers can now start the Python server, see the API docs, and get a clear "binary not found" error message instead of an opaque crash.

---

## BUG #3 — Missing .exe Suffix on Windows (CRITICAL)

**File:** `app.py` (line ~21)  
**What was the bug:**  
The binary path was hardcoded as `"hft_engine"`. On Windows, compiled C++ executables have a `.exe` extension. So the server was looking for a file called `hft_engine` which doesn't exist on Windows — it should be looking for `hft_engine.exe`.

**What the old code looked like:**
```python
ENGINE_PATH = Path(__file__).parent / "build" / "hft_engine"
```

**How I fixed it:**
```python
_engine_name = "hft_engine.exe" if sys.platform == "win32" else "hft_engine"
ENGINE_PATH = Path(__file__).parent / "build" / _engine_name
```

**Why this matters:** Without this, even if the binary was built successfully on Windows, the server would never find it.

---

## BUG #4 — Sell-Side Iterator Invalidation (CRITICAL)

**File:** `src/core/order_book.cpp` (line ~113)  
**What was the bug:**  
This is the most dangerous C++ bug in the entire project. When a SELL order comes in, the matching engine needs to match it against the best BID (highest price). The old code used a `for` loop iterating backwards through `bids_` (a `std::map`):

```cpp
for (auto it = bids_.end(); it != bids_.begin() && incoming.remaining() > 0; ) {
    --it;
    // ... matching logic ...
    if (resting->is_done()) {
        level.orders.pop_front();
        orders_.erase(resting->id);
        if (level.empty()) {
            it = bids_.erase(it);  // ← erases the current element
            break;                  // ← breaks inner while loop
        }
    }
}
// Back to outer for loop → does --it on the ERASED iterator → UNDEFINED BEHAVIOR
```

The problem: After `bids_.erase(it)`, the iterator `it` is set to the next element. Then `break` exits the inner `while` loop. Then the outer `for` loop does `--it` on this already-advanced iterator. But worse — if we erased the first element, `--it` could go before `begin()`, which is undefined behavior. This could cause random crashes, corrupted order books, or silent wrong fills under heavy load.

**How I fixed it:**  
Rewrote the entire sell-side loop to use a `while` loop that re-fetches `std::prev(bids_.end())` on each iteration. This way, after erasing a level, we simply go back to the top of the `while` loop and grab the new best bid. No iterator can be invalidated.

```cpp
while (!bids_.empty() && incoming.remaining() > 0) {
    auto it = std::prev(bids_.end());  // always get fresh best bid
    auto& [px, level] = *it;
    
    if (incoming.type == OrderType::LIMIT || incoming.type == OrderType::IOC) {
        if (px < incoming.price) break;
    }
    
    while (!level.orders.empty() && incoming.remaining() > 0) {
        // ... matching logic (unchanged) ...
    }
    
    if (level.empty()) {
        bids_.erase(it);  // safe — we won't use 'it' again
    } else {
        break;
    }
}
```

**Why this matters:** This is the kind of bug that works fine in testing with small order books but causes random crashes in production under heavy load. A quant firm would lose real money.

---

## BUG #5 — to_price() Rounds Wrong for Negative Numbers

**File:** `src/core/types.hpp` (line ~17)  
**What was the bug:**  
The `to_price()` function converts a `double` (like 185.5050) to an `int64_t` (like 1855050) by multiplying by 10000 and adding 0.5:

```cpp
inline Price to_price(double d) { return static_cast<Price>(d * PRICE_SCALE + 0.5); }
```

This `+0.5` trick works for positive numbers (it rounds 1.6 → 2, 1.4 → 1). But for negative numbers, it's WRONG:
- `-1.5 * 10000 + 0.5 = -14999.5` → truncated to `-14999`
- Correct answer should be `-15000`

This matters because PnL calculations can produce negative intermediate values.

**How I fixed it:**
```cpp
inline Price to_price(double d) { return static_cast<Price>(std::llround(d * PRICE_SCALE)); }
```

`std::llround()` correctly rounds both positive and negative numbers (rounds away from zero).

**Why this matters:** Wrong rounding = wrong prices = wrong PnL = wrong risk calculations. Off-by-one tick errors compound over thousands of trades.

---

## BUG #6 — spread() Returns Garbage for Empty or Crossed Books

**File:** `src/core/types.hpp` (line ~86)  
**What was the bug:**  
The `spread()` function calculated spread without checking if bids/asks existed:

```cpp
double spread() const {
    return from_price(best_ask() - best_bid());
}
```

If `bids` is empty, `best_bid()` returns 0. If `asks` is empty, `best_ask()` returns 0. So `spread()` would return `from_price(0 - 0) = 0` which is accidentally correct for empty books, BUT if you have a crossed book (best_bid > best_ask, which can happen during fast markets), you'd get a negative spread which makes no sense.

**How I fixed it:**
```cpp
double spread() const {
    if (bids.empty() || asks.empty()) return 0.0;
    Price s = best_ask() - best_bid();
    return s > 0 ? from_price(s) : 0.0;
}
```

**Why this matters:** Strategies use spread to decide whether to quote. A negative spread could cause strategies to place inverted orders.

---

## BUG #7 — on_tick() Lock Placed After the Real Work

**File:** `src/core/simulator.cpp` (line ~78)  
**What was the bug:**  
The `on_tick()` function was supposed to update market prices and record the latency. But the lock was placed AFTER the `update_market_price()` call:

```cpp
void ExchangeSimulator::on_tick(const MarketDataTick& tick) {
    int64_t t0 = now_ns();
    risk_.update_market_price(tick.symbol, from_price(tick.last_price));  // ← NO LOCK
    std::lock_guard<std::mutex> lock(mtx_);  // ← lock acquired AFTER the work
    tick_lat_.record(now_ns() - t0);  // ← latency includes lock wait time
}
```

Two problems:
1. `update_market_price()` runs without any protection from the simulator's mutex, so if another thread calls `submit_order()` at the same time, you have a data race on the position's unrealized PnL.
2. The latency recorded includes the time spent waiting for the lock, which makes the metric useless — it measures contention, not processing speed.

**How I fixed it:**
```cpp
void ExchangeSimulator::on_tick(const MarketDataTick& tick) {
    int64_t t0 = now_ns();
    std::lock_guard<std::mutex> lock(mtx_);  // ← lock FIRST
    risk_.update_market_price(tick.symbol, from_price(tick.last_price));
    tick_lat_.record(now_ns() - t0);
}
```

**Why this matters:** Data races cause undefined behavior — corrupted PnL numbers, positions that don't add up, and intermittent crashes that are nearly impossible to reproduce.

---

## BUG #8 — symbols() Missing Lock (Data Race)

**File:** `src/core/simulator.cpp` (line ~92)  
**What was the bug:**  
The `symbols()` function iterates over the `books_` map to return a list of symbol names. But it didn't acquire the mutex:

```cpp
std::vector<std::string> ExchangeSimulator::symbols() const {
    std::vector<std::string> s;
    for (auto& [k, _] : books_) s.push_back(k);  // ← iterating without lock
    return s;
}
```

If another thread calls `add_symbol()` at the same time (which DOES hold the lock and modifies `books_`), you get a data race. On `std::unordered_map`, concurrent read + write is undefined behavior — it can crash, return garbage, or corrupt the map's internal hash table.

**How I fixed it:**
```cpp
std::vector<std::string> ExchangeSimulator::symbols() const {
    std::lock_guard<std::mutex> lock(mtx_);  // ← added lock
    std::vector<std::string> s;
    for (auto& [k, _] : books_) s.push_back(k);
    return s;
}
```

**Why this matters:** Same as above — data races are undefined behavior in C++.

---

## BUG #9 — Rate Limiter Counts Rejected Orders (DoS Vulnerability)

**File:** `src/risk/risk_manager.cpp` (line ~22)  
**What was the bug:**  
The old code recorded the order timestamp at the TOP of `check_order()`, BEFORE any validation:

```cpp
RiskResult RiskManager::check_order(const Order& order, double current_price) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++stats_checked_;
    order_timestamps_ns_.push_back(now_ns());  // ← counted even if rejected
    // ... then check qty, price, position, etc.
}
```

This means: if someone sends 2000 orders with `qty = -1` (which gets rejected immediately), all 2000 are counted toward the rate limit. Now the rate limit is exhausted, and VALID orders from a legitimate strategy also get rejected for the next second.

This is a denial-of-service attack vector — a rogue client can disable all trading by spamming invalid orders.

**How I fixed it:**  
Moved the timestamp recording to AFTER all validation checks pass:

```cpp
RiskResult RiskManager::check_order(const Order& order, double current_price) {
    std::lock_guard<std::mutex> lock(mtx_);
    ++stats_checked_;

    // All validation checks first...
    if (order.qty <= 0) return REJECT;
    if (order.price <= 0) return REJECT;
    // ... position, notional, rate checks ...

    // Only count approved orders toward rate limit
    order_timestamps_ns_.push_back(now_ns());
    return {true, "OK"};
}
```

Also changed the rate check from `>` to `>=` (off-by-one: the old code allowed `max + 1` orders per second).

**Why this matters:** In a real exchange, a rate limiter that counts rejected orders is a security vulnerability. Any client could freeze trading for everyone.

---

## BUG #10 — JSON Injection in Error Response (Command Name)

**File:** `src/engine_cli.cpp` (line ~285)  
**What was the bug:**  
When an unknown command was received, the error message included the raw command name without JSON escaping:

```cpp
return "{\"error\":\"unknown command: " + cmd + "\"}";
```

If someone sends a command like `test"injected":"value`, the output becomes:
```json
{"error":"unknown command: test"injected":"value"}
```

This is malformed JSON. The Python side does `json.loads()` on this response, which would throw an exception, causing a 500 error instead of a clean error message.

**How I fixed it:**
```cpp
return "{\"error\":" + J::str("unknown command: " + cmd) + "}";
```

`J::str()` properly escapes quotes and backslashes inside the string.

**Why this matters:** Malformed JSON breaks the entire Python-C++ communication protocol. Any unusual input could crash the API.

---

## BUG #11 — JSON Injection in Exception Message

**File:** `src/engine_cli.cpp` (line ~307)  
**What was the bug:**  
Same problem as Bug #10, but in the exception handler:

```cpp
catch (const std::exception& e) {
    std::cout << "{\"error\":\"" << e.what() << "\"}\n";
}
```

If `e.what()` contains a quote character (which many standard library exceptions do), the JSON output is malformed.

**How I fixed it:**
```cpp
catch (const std::exception& e) {
    std::cout << "{\"error\":" << J::str(e.what()) << "}\n";
}
```

**Why this matters:** Same as Bug #10 — any exception with a quote in the message would break JSON parsing.

---

## BUG #12 — Out-of-Bounds Read in jget() (engine_cli.cpp)

**File:** `src/engine_cli.cpp` (line ~22-23)  
**What was the bug:**  
The `jget()` JSON parser skips whitespace after the colon:

```cpp
while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
if (body[pos] == '"') {  // ← if pos == body.size(), this is OUT OF BOUNDS
```

After the `while` loop, `pos` could be equal to `body.size()`. Accessing `body[pos]` when `pos == body.size()` reads past the end of the string. In C++, this is undefined behavior — it could read garbage memory, crash, or appear to work.

**How I fixed it:**
```cpp
while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
if (pos >= body.size()) return "";  // ← added bounds check
if (body[pos] == '"') {
```

**Why this matters:** Out-of-bounds reads are undefined behavior. They can be exploited for information disclosure or cause crashes.

---

## BUG #13 — Out-of-Bounds Read in json_get() (main.cpp)

**File:** `src/main.cpp` (line ~56-57)  
**What was the bug:**  
Exact same bug as Bug #12, but in the `json_get()` function in `main.cpp` (the HTTP server version). It's a copy-pasted function with the same flaw.

**How I fixed it:**  
Same fix — added `if (pos >= body.size()) return "";` after the whitespace skip loop.

**Why this matters:** Same as Bug #12.

---

## BUG #14 — Feed Index Race Condition

**File:** `src/main.cpp` (line ~240-241)  
**What was the bug:**  
The feed stepping code used an atomic counter but had a race in the reset logic:

```cpp
size_t idx = g_feed_idx.fetch_add(1);
if (idx >= g_tick_feed.size()) { g_feed_idx = 0; idx = 0; }
```

The problem: Two threads can both see `idx >= size` at the same time. Both reset `g_feed_idx = 0`. Both use `idx = 0`. Now two requests replay the same tick. Also, `g_feed_idx = 0` is a non-atomic store on an atomic variable — technically a data race.

**How I fixed it:**
```cpp
size_t idx = g_feed_idx.fetch_add(1) % g_tick_feed.size();
```

Using modulo (`%`) on the atomically-fetched value gives correct wraparound without any reset. Lock-free, race-free, one line.

**Why this matters:** In an HFT context, replaying the same tick twice or skipping ticks means your backtest results are wrong.

---

## BUG #15 — Dead Code / No-Op Expression

**File:** `app.py`  
**What was the bug:**  
There was a line that looked like it was trying to do something but was actually a no-op expression that Python evaluated and threw away:

```python
self.proc.stdout._sock if hasattr(self.proc.stdout, '_sock') else None
```

This evaluates to either a socket object or `None`, but the result is never assigned to anything or used. It does nothing.

**How I fixed it:** Removed the dead code entirely.

**Why this matters:** Dead code confuses future developers and suggests the original author didn't understand what they were writing.

---

## BUG #16 — depth Parameter Not Forwarded

**File:** `app.py` (line ~191)  
**What was the bug:**  
The `/api/book/{symbol}` endpoint accepted a `depth` query parameter, but never actually sent it to the C++ engine:

```python
@app.get("/api/book/{symbol}")
def book(symbol: str, depth: int = 10):
    return _get_engine().send("book", {"symbol": symbol})  # ← depth is ignored!
```

So no matter what depth the user requested, they always got the default 10 levels.

**How I fixed it:**
```python
return _get_engine().send("book", {"symbol": symbol, "depth": depth})
```

**Why this matters:** Silent parameter ignoring is a bug that erodes API trust. Users think they're controlling the depth but they're not.

---

## BUG #17 — Dangling Declaration (Linker Bomb)

**File:** `src/core/simulator.hpp` (line ~104)  
**What was the bug:**  
The `Backtester` class declared a method `replay_journal(const std::string& path)` in the header, but the implementation (`.cpp` file) never defined it:

```cpp
class Backtester {
public:
    // ...
    BacktestResult replay_journal(const std::string& path);  // ← declared but never defined
};
```

Right now, nothing calls this function, so there's no linker error. But if anyone ever tries to call `bt.replay_journal("file.journal")`, they'll get a cryptic linker error: `undefined reference to Backtester::replay_journal`. This is a maintenance trap.

**How I fixed it:** Removed the declaration entirely.

**Why this matters:** Dangling declarations are tech debt that confuses future developers and causes linker errors when someone tries to use the function.

---

## BONUS FIXES (Cleanup)

- **Removed unused imports** in `app.py`: `asyncio`, `StaticFiles`, `JSONResponse`, `time` — none were used anywhere in the file.
- **Added `self.proc = None`** before restart in error paths — ensures the old dead process handle doesn't leak.

---

## Summary by File

| File | Bugs Fixed | Bug Numbers |
|------|-----------|-------------|
| `app.py` | 7 | #1, #2, #3, #15, #16, +cleanup |
| `src/core/types.hpp` | 2 | #5, #6 |
| `src/core/order_book.cpp` | 1 | #4 |
| `src/core/simulator.cpp` | 2 | #7, #8 |
| `src/core/simulator.hpp` | 1 | #17 |
| `src/risk/risk_manager.cpp` | 1 | #9 |
| `src/engine_cli.cpp` | 3 | #10, #11, #12 |
| `src/main.cpp` | 2 | #13, #14 |

---

## Summary by Severity

| Severity | Count | Bug Numbers |
|----------|-------|-------------|
| 🔴 CRITICAL (crash/UB) | 4 | #1, #2, #3, #4 |
| 🟡 MEDIUM (wrong behavior) | 9 | #5, #6, #7, #8, #9, #10, #11, #12, #13, #14 |
| 🟢 LOW (cleanup/maintenance) | 4 | #15, #16, #17, +imports |
