# market_data_bridge.py — AKShare→TCP length-prefixed JSON bridge
#
# Protocol (matching C++ DataIngestor):
#   [4 bytes: uint32_t payload length (network byte order)]
#   [N bytes: JSON payload]
#
# Uses curl subprocess for HTTP (trusted proxy compatibility).
#
# Usage:
#   HTTPS_PROXY=http://127.0.0.1:7890 python3 py/bridge/market_data_bridge.py
#   DataIngestor connects to localhost:9000

import argparse
import asyncio
import json
import logging
import os
import struct
import subprocess
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

logging.basicConfig(level=logging.INFO, format="[Bridge] %(message)s")
log = logging.getLogger(__name__)

PROXY = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") or ""


# ── Curl-based data fetcher ──

def fetch_eastmoney_kline(symbol_raw: str, days: int = 5):
    """Pull recent daily kline from eastmoney API via curl.

    Eastmoney API: push2his.eastmoney.com
    secid: 0.{code} for SZ, 1.{code} for SH
    """
    code = symbol_raw.replace(".SZ", "").replace(".SH", "")
    market = "0" if symbol_raw.endswith("SZ") else "1"
    secid = f"{market}.{code}"

    today = datetime.now()
    beg = (today - timedelta(days=days + 3)).strftime("%Y%m%d")
    end = today.strftime("%Y%m%d")

    url = (
        f"https://push2his.eastmoney.com/api/qt/stock/kline/get?"
        f"fields1=f1,f2,f3,f4,f5,f6"
        f"&fields2=f51,f52,f53,f54,f55,f56,f57"
        f"&ut=7eea3edcaed734bea9cbfc24409ed989"
        f"&klt=101&fqt=1&secid={secid}&beg={beg}&end={end}"
    )

    cmd = ["curl", "-s", "--max-time", "15"]
    if PROXY:
        cmd += ["-x", PROXY]
    cmd.append(url)

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        if result.returncode != 0:
            log.debug(f"curl failed for {symbol_raw}: {result.stderr[:100]}")
            return []
        data = json.loads(result.stdout)
        klines = data.get("data", {}).get("klines", [])
        if not klines:
            return []

        results = []
        for line in klines:
            parts = line.split(",")
            if len(parts) < 7:
                continue
            # parts: date,open,high,low,close,volume,amount
            ts = int(datetime.strptime(parts[0], "%Y-%m-%d").timestamp() * 1_000_000)
            results.append({
                "symbol": symbol_raw,
                "ts": ts,
                "open": float(parts[1]),
                "high": float(parts[2]),
                "low": float(parts[3]),
                "close": float(parts[4]),
                "volume": int(float(parts[5])),
                "amount": int(float(parts[6])),
            })
        return results
    except Exception as e:
        log.warning(f"fetch failed for {symbol_raw}: {e}")
        return []


# ── TCP Bridge Server ──

async def handle_client(reader, writer, state):
    addr = writer.get_extra_info("peername")
    log.info(f"DataIngestor connected from {addr}")

    try:
        for sym in state["symbols"]:
            rows = fetch_eastmoney_kline(sym, days=state["lookback_days"])
            for row in rows:
                payload = json.dumps(row).encode()
                header = struct.pack("!I", len(payload))
                writer.write(header + payload)

        await writer.drain()
        total = sum(1 for sym in state["symbols"]
                    for _ in fetch_eastmoney_kline(sym, days=1))
        log.info(f"Sent batch for {len(state['symbols'])} symbols to {addr}")

        # Keep connection alive, push periodic updates
        while True:
            try:
                data = await asyncio.wait_for(reader.read(1024),
                                               timeout=state["poll_interval"])
                if not data:
                    break
            except asyncio.TimeoutError:
                for sym in state["symbols"]:
                    rows = fetch_eastmoney_kline(sym, days=1)
                    for row in rows:
                        payload = json.dumps(row).encode()
                        header = struct.pack("!I", len(payload))
                        writer.write(header + payload)
                await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()
        log.info(f"DataIngestor disconnected from {addr}")


async def run_bridge(host, port, symbols, freq, lookback_days, poll_sec):
    state = {
        "symbols": symbols,
        "freq": freq,
        "lookback_days": lookback_days,
        "poll_interval": poll_sec,
    }
    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, state), host, port
    )
    log.info(f"Bridge listening on {host}:{port}")
    log.info(f"Symbols: {symbols}, freq={freq}, poll={poll_sec}s")
    log.info(f"Proxy: {PROXY or 'DIRECT'}")
    async with server:
        await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="Market Data Bridge (curl-based)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--symbols", default="000001.SZ,600519.SH,300750.SZ")
    parser.add_argument("--freq", default="daily")
    parser.add_argument("--lookback", type=int, default=5)
    parser.add_argument("--poll", type=int, default=60)
    args = parser.parse_args()

    symbols = [s.strip() for s in args.symbols.split(",")]
    log.info("Starting Market Data Bridge (curl-based, eastmoney API)")

    try:
        asyncio.run(run_bridge(args.host, args.port, symbols, args.freq,
                                args.lookback, args.poll))
    except KeyboardInterrupt:
        log.info("Bridge stopped")


if __name__ == "__main__":
    main()
