#!/usr/bin/env python3
"""
Batch download CSI300 + CSI500 daily K-line data (~800 stocks, last month).

Uses direct Tencent finance API (bypasses AKShare wrapper which has init_start_date bug).
Output: data/csv_daily/{code}.csv  — one file per stock.
CSV format: symbol,date,open,high,low,close,volume,amount
"""
import akshare as ak
import csv
import json
import os
import sys
import time
import urllib.request
from datetime import datetime, timedelta

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "data", "csv_daily")
os.makedirs(DATA_DIR, exist_ok=True)

# ── Clear proxy (system proxy 127.0.0.1:7890 is not accessible) ──
for var in ["HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
            "HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
            "NO_PROXY", "no_proxy"]:
    os.environ.pop(var, None)

# ── Stock code → market prefix mapping ──
def market_prefix(code: str) -> str:
    """Determine Tencent API market prefix for an A-share code."""
    if code.startswith("6") or code.startswith("688"):
        return "sh"
    return "sz"


def market_suffix(code: str) -> str:
    """Determine CSV symbol suffix for an A-share code."""
    if code.startswith("6") or code.startswith("688"):
        return ".SH"
    return ".SZ"


# ── Fetch daily K-line from Tencent API ──
def fetch_tencent_kline(code: str, start_date: str, end_date: str) -> list:
    """
    Fetch daily K-line from Tencent finance API.
    Returns list of [date, open, close, high, low, volume] strings.
    Uses urllib (stdlib, no extra deps).
    """
    prefix = market_prefix(code)
    url = (f"http://ifzq.gtimg.cn/appstock/app/fqkline/get?"
           f"param={prefix}{code},day,{start_date},{end_date},500,qfq")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        print(f"    HTTP error: {e}")
        return []

    if data.get("code") != 0:
        return []

    kline = data.get("data", {}).get(f"{prefix}{code}", {}).get("qfqday", [])
    return kline  # [date, open, close, high, low, volume]


# ── Step 1: Fetch constituent lists ──
print("[1/4] Fetching CSI 300 constituent list...")
df300 = ak.index_stock_cons(symbol="000300")
codes_300 = set(df300["品种代码"].tolist())
print(f"  CSI 300: {len(codes_300)} stocks")

print("[2/4] Fetching CSI 500 constituent list...")
df500 = ak.index_stock_cons(symbol="000905")
codes_500 = set(df500["品种代码"].tolist())
print(f"  CSI 500: {len(codes_500)} stocks")

all_codes = sorted(codes_300 | codes_500)
print(f"  Total unique: {len(all_codes)} stocks")

# ── Step 2: Date range ──
end_date = datetime.now().strftime("%Y-%m-%d")
start_date = (datetime.now() - timedelta(days=35)).strftime("%Y-%m-%d")
print(f"[3/4] Date range: {start_date} ~ {end_date}\n")

# ── Step 3: Batch download ──
print(f"[4/4] Downloading daily K-lines ({len(all_codes)} stocks)...")
succeeded = 0
failed = 0
skipped = 0
total = len(all_codes)
t0 = time.time()

for idx, code in enumerate(all_codes):
    out_path = os.path.join(DATA_DIR, f"{code}.csv")
    if os.path.exists(out_path) and os.path.getsize(out_path) > 100:
        skipped += 1
        if idx % 50 == 0 or idx == total - 1:
            elapsed = time.time() - t0
            print(f"  [{idx+1}/{total}] {code}: skipped ({elapsed:.0f}s elapsed)")
        continue

    kline = fetch_tencent_kline(code, start_date, end_date)
    if not kline:
        failed += 1
        if idx % 10 == 0 or idx == total - 1:
            print(f"  [{idx+1}/{total}] {code}: no data")
        time.sleep(0.5)
        continue

    suffix = market_suffix(code)
    symbol = f"{code}{suffix}"

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["symbol", "date", "open", "high", "low", "close", "volume", "amount"])
        for row in kline:
            # Tencent format: [date, open, close, high, low, volume]
            date_str = row[0]
            open_p = float(row[1])
            close_p = float(row[2])
            high_p = float(row[3])
            low_p = float(row[4])
            vol = int(float(row[5])) if row[5] else 0
            writer.writerow([symbol, date_str, open_p, high_p, low_p, close_p, vol, 0])

    succeeded += 1
    if idx % 10 == 0 or idx == total - 1:
        elapsed = time.time() - t0
        eta = (elapsed / (idx + 1)) * (total - idx - 1) if idx > 0 else 0
        print(f"  [{idx+1}/{total}] {code}: {len(kline)} rows OK ({elapsed:.0f}s, ETA {eta:.0f}s)")

    time.sleep(0.5)  # Rate limiting

# ── Summary ──
elapsed = time.time() - t0
print(f"\n{'='*50}")
print(f"Done in {elapsed:.0f}s.")
print(f"  Succeeded: {succeeded}  Skipped: {skipped}  Failed: {failed}")
total_files = succeeded + skipped
print(f"  Total CSV files: {total_files}")
data_size = sum(os.path.getsize(os.path.join(DATA_DIR, f"{c}.csv"))
                for c in all_codes
                if os.path.exists(os.path.join(DATA_DIR, f"{c}.csv")))
print(f"  Data size: {data_size / 1024:.0f} KB")
print(f"  Output: {DATA_DIR}")
print(f"{'='*50}")
