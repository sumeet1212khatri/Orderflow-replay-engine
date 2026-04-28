"""
HFT Exchange Simulator — FastAPI server
Wraps the C++ hft_engine binary via persistent subprocess.
"""
import json
import os
import subprocess
import sys
import threading
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

# BUG-FIX #1: Detect platform and pick the correct binary name.
# On Windows the compiled binary has a .exe extension.
_engine_name = "hft_engine.exe" if sys.platform == "win32" else "hft_engine"
ENGINE_PATH = Path(__file__).parent / "build" / _engine_name
ENGINE_TIMEOUT = 10.0


class EngineProcess:
    """Manages a persistent child process that speaks line-delimited JSON."""

    def __init__(self):
        self.proc: Optional[subprocess.Popen] = None
        self.lock = threading.Lock()
        self._start()

    # ------------------------------------------------------------------
    def _start(self):
        if not ENGINE_PATH.exists():
            # BUG-FIX #2: Do not crash the whole server if the binary is
            # missing — let individual requests report the problem instead.
            self.proc = None
            return
        self.proc = subprocess.Popen(
            [str(ENGINE_PATH)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )

    # ------------------------------------------------------------------
    def send(self, cmd: str, payload: dict = None) -> dict:
        if payload is None:
            payload = {}
        line = f"{cmd} {json.dumps(payload)}\n"

        with self.lock:
            # BUG-FIX #3: If the engine binary was never found, report it
            # clearly instead of an opaque NoneType crash.
            if self.proc is None or self.proc.poll() is not None:
                self._start()
                if self.proc is None:
                    raise HTTPException(
                        503,
                        f"Engine binary not found at {ENGINE_PATH}. "
                        "Build the C++ engine first.",
                    )

            try:
                self.proc.stdin.write(line)
                self.proc.stdin.flush()

                # BUG-FIX #4 (THE CRASH BUG):
                # The old code used `select.select()` which ONLY works with
                # sockets on Windows — it raises OSError on pipe file
                # descriptors.  Replaced with a cross-platform thread-based
                # readline with timeout.
                response_container: list = []
                error_container: list = []

                def _read():
                    try:
                        resp = self.proc.stdout.readline()
                        response_container.append(resp)
                    except Exception as exc:
                        error_container.append(exc)

                reader = threading.Thread(target=_read, daemon=True)
                reader.start()
                reader.join(timeout=ENGINE_TIMEOUT)

                if reader.is_alive():
                    # Timed out — kill and restart the engine
                    self.proc.kill()
                    self.proc = None
                    self._start()
                    raise HTTPException(504, "Engine timeout — restarted")

                if error_container:
                    raise error_container[0]

                response = (
                    response_container[0].strip() if response_container else ""
                )
                if not response:
                    raise HTTPException(500, "Engine returned empty response")

                return json.loads(response)

            except HTTPException:
                raise
            except Exception as e:
                try:
                    self.proc.kill()
                except Exception:
                    pass
                self.proc = None
                self._start()
                raise HTTPException(500, f"Engine error: {e}")


# BUG-FIX #5: Lazy-initialise so the import doesn't blow up when running
# under test-harnesses or when the binary hasn't been built yet.
engine: Optional[EngineProcess] = None


def _get_engine() -> EngineProcess:
    global engine
    if engine is None:
        engine = EngineProcess()
    return engine


app = FastAPI(title="HFT Exchange Simulator", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ── Pydantic models ──────────────────────────────────────────────────────────

class OrderRequest(BaseModel):
    symbol: str = "AAPL"
    side: str = "BUY"
    type: str = "LIMIT"
    price: float = 0.0
    qty: int = 100
    client_id: str = ""
    market_price: float = 0.0


class CancelRequest(BaseModel):
    symbol: str
    id: int


class FeedStepRequest(BaseModel):
    n: int = 10


class BacktestRequest(BaseModel):
    strategy: str = "MarketMaking"
    symbol: str = "AAPL"
    ticks: int = 5000
    start_price: float = 185.0


class RiskLimitsRequest(BaseModel):
    max_order_qty: int = 10000
    max_position: int = 500000
    max_notional_usd: float = 50_000_000.0
    max_orders_per_sec: int = 2000


# ── API routes ────────────────────────────────────────────────────────────────

@app.get("/api/status")
def status():
    return _get_engine().send("status")


@app.get("/api/symbols")
def symbols():
    return _get_engine().send("symbols")


@app.get("/api/book/{symbol}")
def book(symbol: str, depth: int = 10):
    # BUG-FIX #6: The `depth` query-param was accepted but never forwarded
    # to the engine.  Now included in the payload.
    return _get_engine().send("book", {"symbol": symbol, "depth": depth})


@app.get("/api/positions")
def positions():
    return _get_engine().send("positions")


@app.get("/api/trades")
def trades():
    return _get_engine().send("trades")


@app.get("/api/orders")
def orders():
    return _get_engine().send("orders")


@app.post("/api/order")
def submit_order(req: OrderRequest):
    return _get_engine().send("order", req.model_dump())


@app.delete("/api/order")
def cancel_order(req: CancelRequest):
    return _get_engine().send("cancel", req.model_dump())


@app.post("/api/feed/step")
def feed_step(req: FeedStepRequest):
    return _get_engine().send("feed_step", {"n": req.n})


@app.post("/api/backtest")
def backtest(req: BacktestRequest):
    return _get_engine().send("backtest", req.model_dump())


@app.get("/api/risk/limits")
def get_limits():
    return _get_engine().send("get_limits")


@app.post("/api/risk/limits")
def set_limits(req: RiskLimitsRequest):
    return _get_engine().send("risk_limits", req.model_dump())


@app.post("/api/reset")
def reset():
    return _get_engine().send("reset")


@app.get("/")
def serve_index():
    return FileResponse(str(Path(__file__).parent / "index.html"))


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn
    port = int(os.environ.get("PORT", 7860))
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info")
