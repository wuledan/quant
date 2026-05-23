#!/usr/bin/env python3
"""
Download corporate actions (dividends) for all A-share stocks via AKShare.

Output:
  data/corporate_actions.csv — human-readable summary
  data/corp_actions.bin     — binary format consumed by CorporateActionStore

Binary format:
  Header: 64 bytes (magic=0x434F5250, version=1, num_actions, reserved)
  Records (per action):
    uint16_t slen        — symbol length
    char symbol[slen]    — symbol string (e.g. "000001.SZ")
    int64_t action_date  — epoch microseconds
    uint8_t type         — 0=dividend, 1=split, 2=rights, 3=suspension
    double value         — dividend per share (yuan)
    double adjust_factor — backward price adjustment factor
"""
import csv
import os
import struct
import time
from datetime import datetime, timedelta, timezone

import akshare as ak
import pandas as pd

# ── Clear proxy ──
for var in ["HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
            "NO_PROXY", "no_proxy"]:
    os.environ.pop(var, None)

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data")
CSV_DAILY_DIR = os.path.join(DATA_DIR, "csv_daily")
OUTPUT_CSV = os.path.join(DATA_DIR, "corporate_actions.csv")
OUTPUT_BIN = os.path.join(DATA_DIR, "corp_actions.bin")


def date_to_epoch_us(date_str: str) -> int:
    dt = datetime.strptime(date_str, "%Y-%m-%d")
    return int(dt.replace(tzinfo=timezone.utc).timestamp() * 1_000_000)


def epoch_us_to_date(ts_us: int) -> str:
    dt = datetime.fromtimestamp(ts_us / 1_000_000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d")


def strip_market_suffix(symbol: str) -> str:
    return symbol.split(".")[0]


def compute_adjust_factor(dividend_per_share: float, close_price: float) -> float:
    """Backward adjustment factor for cash dividend."""
    if close_price <= 0 or dividend_per_share <= 0:
        return 1.0
    adj = close_price / (close_price - dividend_per_share)
    return max(1.0, min(adj, 1.5))


# ── Main ──
def main():
    # ── Step 1: Single pass — read all CSVs for symbols + price lookup ──
    print("[1/3] Reading CSV files (single pass)...", flush=True)
    symbols = set()
    price_lookup = {}  # symbol -> {date_str -> close_price}

    csv_files = sorted(f for f in os.listdir(CSV_DAILY_DIR) if f.endswith(".csv"))
    total_files = len(csv_files)
    for idx, fname in enumerate(csv_files, 1):
        fpath = os.path.join(CSV_DAILY_DIR, fname)
        with open(fpath, newline="") as f:
            reader = csv.reader(f)
            try:
                header = next(reader)
            except StopIteration:
                continue
            for row in reader:
                if len(row) < 6:
                    continue
                sym = row[0]
                date_str = row[1]
                symbols.add(sym)
                try:
                    close = float(row[5])
                except (ValueError, IndexError):
                    continue
                if sym not in price_lookup:
                    price_lookup[sym] = {}
                price_lookup[sym][date_str] = close
        if idx % 100 == 0 or idx == total_files:
            print(f"  ... {idx}/{total_files} files parsed, {len(symbols)} symbols so far", flush=True)

    symbols = sorted(symbols)
    print(f"  Done: {len(symbols)} unique symbols, {len(price_lookup)} with price data", flush=True)

    # ── Step 2: Download dividend data via AKShare ──
    print("[2/3] Downloading dividend data via AKShare...")
    all_actions = []  # (symbol, date_us, type, value, adjust_factor)

    total = len(symbols)
    for idx, sym in enumerate(symbols, 1):
        code = strip_market_suffix(sym)
        try:
            df = ak.stock_history_dividend_detail(symbol=code, indicator="分红")
        except Exception as e:
            if idx % 50 == 0 or idx == 1 or idx == total:
                print(f"  [{idx}/{total}] {sym}: error: {e}")
            continue

        if df.empty:
            if idx % 50 == 0 or idx == total:
                print(f"  [{idx}/{total}] {sym}: no dividend data")
            continue

        div_df = df[df["进度"] == "实施"].copy()
        if div_df.empty:
            if idx % 50 == 0 or idx == total:
                print(f"  [{idx}/{total}] {sym}: no executed dividends")
            continue

        count = 0
        for _, row in div_df.iterrows():
            ex_div_date = row.get("除权除息日")
            if pd.isna(ex_div_date) or not ex_div_date:
                continue
            date_str = str(ex_div_date)[:10]

            div_per_10 = float(row.get("派息", 0))
            if div_per_10 <= 0:
                continue
            div_per_share = div_per_10 / 10.0

            # Find close price for adjust_factor
            close_price = 0
            if sym in price_lookup and date_str in price_lookup[sym]:
                close_price = price_lookup[sym][date_str]
            else:
                dt = datetime.strptime(date_str, "%Y-%m-%d")
                for offset in range(1, 10):
                    prev = (dt - timedelta(days=offset)).strftime("%Y-%m-%d")
                    if sym in price_lookup and prev in price_lookup[sym]:
                        close_price = price_lookup[sym][prev]
                        break

            adj_factor = compute_adjust_factor(div_per_share, close_price) if close_price > 0 else 1.0
            date_us = date_to_epoch_us(date_str)
            all_actions.append((sym, date_us, 0, div_per_share, adj_factor))
            count += 1

        if idx % 50 == 0 or idx == 1 or idx == total:
            print(f"  [{idx}/{total}] {sym}: {count} dividends")
        # Rate limit: 0.3s between API calls
        if idx < total:
            time.sleep(0.3)

    print(f"\n  Total dividend actions: {len(all_actions)}", flush=True)

    # ── Step 3: Write outputs ──
    print("[3/3] Writing output files...", flush=True)

    # CSV
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["symbol", "date", "type", "value", "adjust_factor"])
        for sym, date_us, atype, val, adj in sorted(all_actions, key=lambda x: (x[0], x[1])):
            date_str = epoch_us_to_date(date_us)
            type_str = ["dividend", "split", "rights", "suspension"][atype]
            writer.writerow([sym, date_str, type_str, val, adj])
    print(f"  CSV: {OUTPUT_CSV}")

    # Binary
    with open(OUTPUT_BIN, "wb") as f:
        f.write(struct.pack("<II", 0x434F5250, 1))  # magic, version
        f.write(struct.pack("<I", len(all_actions)))
        f.write(b"\x00" * 52)  # reserved (64 byte header)
        for sym, date_us, atype, val, adj in sorted(all_actions, key=lambda x: (x[0], x[1])):
            sym_bytes = sym.encode("utf-8")
            f.write(struct.pack("<H", len(sym_bytes)))
            f.write(sym_bytes)
            f.write(struct.pack("<q", date_us))
            f.write(struct.pack("<B", atype))
            f.write(struct.pack("<d", val))
            f.write(struct.pack("<d", adj))
    print(f"  Binary: {OUTPUT_BIN} ({os.path.getsize(OUTPUT_BIN)} bytes, {len(all_actions)} actions)")
    print("\nDone.")


if __name__ == "__main__":
    main()
