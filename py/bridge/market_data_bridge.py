# market_data_bridge.py — AKShare(Tencent)→TCP length-prefixed JSON bridge
#
# Protocol (matching C++ DataIngestor):
#   [4 bytes: uint32_t payload length (network byte order)]
#   [N bytes: JSON payload]
#
# Uses AKShare stock_zh_a_hist_tx (Tencent finance) — no token, no proxy needed.
#
# Usage:
#   python3 py/bridge/market_data_bridge.py --port 9000 --symbols 000001.SZ,600519.SH
#   DataIngestor connects to localhost:9000

import argparse
import asyncio
import json
import logging
import os
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

logging.basicConfig(level=logging.INFO, format="[Bridge] %(message)s")
log = logging.getLogger(__name__)

# Clear proxy for CN direct access
for _k in list(os.environ.keys()):
    if 'proxy' in _k.lower():
        del os.environ[_k]

import akshare as ak


def _to_tx_symbol(symbol_raw: str) -> str:
    """Convert 000001.SZ → sz000001, 600519.SH → sh600519"""
    code = symbol_raw.replace(".SZ", "").replace(".SH", "")
    market = "sz" if symbol_raw.endswith("SZ") else "sh"
    return f"{market}{code}"


def fetch_kline(symbol_raw: str, days: int = 5):
    """Pull recent daily kline from Tencent finance via AKShare."""
    tx_code = _to_tx_symbol(symbol_raw)
    today = datetime.now().strftime("%Y%m%d")
    # Look back ~1 year
    start_date = f"{datetime.now().year - 1}0101"
    try:
        df = ak.stock_zh_a_hist_tx(symbol=tx_code, start_date=start_date, end_date=today)
        if df is None or df.empty:
            return []

        rows = []
        for _, row in df.iterrows():
            try:
                ts = int(datetime.strptime(str(row["date"]), "%Y-%m-%d").timestamp() * 1_000_000)
            except Exception:
                continue
            rows.append({
                "symbol": symbol_raw,
                "ts": ts,
                "open": float(row["open"]),
                "high": float(row["high"]),
                "low": float(row["low"]),
                "close": float(row["close"]),
                "volume": int(float(row.get("volume", 0))),
                "amount": int(float(row.get("amount", 0))),
            })
        # Return last N days
        return rows[-days:] if len(rows) > days else rows
    except Exception as e:
        log.warning(f"fetch failed for {symbol_raw}: {e}")
        return []


async def handle_client(reader, writer, state):
    addr = writer.get_extra_info("peername")
    log.info(f"DataIngestor connected from {addr}")

    total = 0
    try:
        for sym in state["symbols"]:
            try:
                rows = fetch_kline(sym, days=state["lookback_days"])
                for row in rows:
                    payload = json.dumps(row).encode()
                    header = struct.pack("!I", len(payload))
                    writer.write(header + payload)
                total += len(rows)
            except Exception as e:
                log.warning(f"fetch {sym}: {e}")

        await writer.drain()
        log.info(f"Sent {total} klines for {len(state['symbols'])} symbols")

        # Keep alive + periodic push
        while True:
            try:
                data = await asyncio.wait_for(reader.read(1024),
                                               timeout=state["poll_interval"])
                if not data:
                    break
            except asyncio.TimeoutError:
                for sym in state["symbols"]:
                    rows = fetch_kline(sym, days=1)
                    for row in rows:
                        payload = json.dumps(row).encode()
                        header = struct.pack("!I", len(payload))
                        writer.write(header + payload)
                await writer.drain()
                log.debug(f"Pushed update")
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()
        log.info(f"DataIngestor disconnected ({total} klines sent)")


async def run_bridge(host, port, symbols, freq, lookback_days, poll_sec):
    state = {
        "symbols": symbols, "freq": freq,
        "lookback_days": lookback_days, "poll_interval": poll_sec,
    }
    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, state), host, port
    )
    log.info(f"Bridge listening on {host}:{port}")
    log.info(f"Source: Tencent Finance (AKShare stock_zh_a_hist_tx)")
    log.info(f"Symbols: {symbols}, poll={poll_sec}s")
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="Market Data Bridge (Tencent/AKShare)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--symbols", default="000001.SZ,600519.SH,300750.SZ")
    parser.add_argument("--freq", default="daily")
    parser.add_argument("--lookback", type=int, default=30)
    parser.add_argument("--poll", type=int, default=60, help="Poll interval (seconds)")
    args = parser.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",")]
    log.info("Starting Market Data Bridge — Tencent Finance")
    try:
        asyncio.run(run_bridge(args.host, args.port, symbols, args.freq,
                                args.lookback, args.poll))
    except KeyboardInterrupt:
        log.info("Bridge stopped")


if __name__ == "__main__":
    main()
