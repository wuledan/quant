# market_data_bridge.py — AKShare → TCP length-prefixed JSON bridge
#
# Protocol (matching C++ DataIngestor):
#   [4 bytes: uint32_t payload length (network byte order)]
#   [N bytes: JSON payload]
#
# JSON format (matching DataIngestor::parse_kline):
#   {"symbol":"000001.SZ","ts":1769030400000000,"open":11.12,"high":11.20,
#    "low":11.08,"close":11.16,"volume":12345678,"amount":137000000}
#
# Usage:
#   python3 py/bridge/market_data_bridge.py --port 9000 --symbols 000001.SZ,600519.SH
#   # DataIngestor connects to localhost:9000

import argparse
import asyncio
import json
import logging
import socket
import struct
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

# Add py/ to path
sys.path.insert(0, str(Path(__file__).parent.parent))

import akshare as ak

logging.basicConfig(level=logging.INFO, format="[Bridge] %(message)s")
log = logging.getLogger(__name__)


# ── AKShare data fetcher ──

def fetch_recent_kline(symbol_raw: str, freq: str = "daily", days: int = 5):
    """Pull recent kline data from AKShare.

    Args:
        symbol_raw: "000001" or "000001.SZ" (market suffix stripped)
        freq: "daily", "weekly", "monthly", "1m", "5m", "15m", "30m", "60m"
        days: lookback window

    Returns:
        list of dict: [{symbol, ts, open, high, low, close, volume, amount}, ...]
    """
    symbol = symbol_raw.replace(".SZ", "").replace(".SH", "")

    today = datetime.now()
    start = (today - timedelta(days=days + 5)).strftime("%Y%m%d")
    end = today.strftime("%Y%m%d")

    try:
        if freq in ("daily", "weekly", "monthly"):
            df = ak.stock_zh_a_hist(
                symbol=symbol, period=freq, start_date=start, end_date=end, adjust="qfq"
            )
            if df is None or df.empty:
                return []

            results = []
            for _, row in df.iterrows():
                date_str = row["日期"].replace("-", "")
                ts = int(
                    datetime.strptime(date_str, "%Y%m%d").timestamp() * 1_000_000
                )
                results.append(
                    {
                        "symbol": symbol_raw,
                        "ts": ts,
                        "open": float(row["开盘"]),
                        "high": float(row["最高"]),
                        "low": float(row["最低"]),
                        "close": float(row["收盘"]),
                        "volume": int(row["成交量"]),
                        "amount": int(float(row["成交额"])),
                    }
                )
            return results
        else:
            # Minute-level: AKShare intraday API
            period_map = {"1m": "1", "5m": "5", "15m": "15", "30m": "30", "60m": "60"}
            period = period_map.get(freq, "5")
            df = ak.stock_zh_a_hist_min_em(
                symbol=symbol, period=period, start_date=start, end_date=end, adjust="qfq"
            )
            if df is None or df.empty:
                return []

            results = []
            for _, row in df.iterrows():
                ts = int(row["时间"].timestamp() * 1_000_000)
                results.append(
                    {
                        "symbol": symbol_raw,
                        "ts": ts,
                        "open": float(row["开盘"]),
                        "high": float(row["最高"]),
                        "low": float(row["最低"]),
                        "close": float(row["收盘"]),
                        "volume": int(row["成交量"]),
                        "amount": int(float(row["成交额"])),
                    }
                )
            return results
    except Exception as e:
        log.warning(f"fetch failed for {symbol_raw}: {e}")
        return []


# ── TCP Bridge Server ──

async def handle_client(reader, writer, state):
    """Accept connection, push data, keep-alive."""
    addr = writer.get_extra_info("peername")
    log.info(f"DataIngestor connected from {addr}")

    try:
        # Send initial batch of recent data
        for sym in state["symbols"]:
            rows = fetch_recent_kline(sym, state["freq"], days=state["lookback_days"])
            for row in rows:
                payload = json.dumps(row).encode()
                header = struct.pack("!I", len(payload))
                writer.write(header + payload)

        await writer.drain()
        log.info(
            f"Sent initial batch for {len(state['symbols'])} symbols to {addr}"
        )

        # Keep connection alive and push periodic updates
        while True:
            try:
                data = await asyncio.wait_for(reader.read(1024), timeout=state["poll_interval"])
                if not data:
                    break
            except asyncio.TimeoutError:
                # Periodic poll
                for sym in state["symbols"]:
                    rows = fetch_recent_kline(sym, state["freq"], days=1)
                    for row in rows:
                        payload = json.dumps(row).encode()
                        header = struct.pack("!I", len(payload))
                        writer.write(header + payload)
                await writer.drain()
                log.debug(f"Pushed update for {len(state['symbols'])} symbols")
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()
        log.info(f"DataIngestor disconnected from {addr}")


async def run_bridge(host, port, symbols, freq, lookback_days, poll_sec):
    """Start TCP bridge server."""
    state = {
        "symbols": symbols,
        "freq": freq,
        "lookback_days": lookback_days,
        "poll_interval": poll_sec,
    }

    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, state), host, port
    )

    addrs = ", ".join(str(s.getsockname()) for s in server.sockets)
    log.info(f"Bridge listening on {addrs}")
    log.info(f"Symbols: {symbols}, freq={freq}, poll={poll_sec}s")

    async with server:
        await server.serve_forever()


# ── CLI ──

def main():
    parser = argparse.ArgumentParser(description="AKShare Market Data Bridge")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument(
        "--symbols",
        default="000001.SZ,600519.SH,000002.SZ,300750.SZ",
        help="Comma-separated symbols",
    )
    parser.add_argument(
        "--freq", default="daily", help="daily|1m|5m|15m|30m|60m"
    )
    parser.add_argument("--lookback", type=int, default=5, help="Initial lookback days")
    parser.add_argument("--poll", type=int, default=60, help="Poll interval (seconds)")
    args = parser.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",")]

    log.info("Starting AKShare Market Data Bridge")
    log.info(f"Source: AKShare (free, no token required)")

    try:
        asyncio.run(
            run_bridge(
                args.host, args.port, symbols, args.freq, args.lookback, args.poll
            )
        )
    except KeyboardInterrupt:
        log.info("Bridge stopped")


if __name__ == "__main__":
    main()
