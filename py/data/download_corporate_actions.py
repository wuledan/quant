#!/usr/bin/env python3
"""
Download corporate actions (dividends) for all A-share stocks via AKShare.

Output:
  data/corporate_actions.csv — human-readable summary
  data/corp_actions.bin     — binary format consumed by CorporateActionStore

Binary format (CorporateActionStore::load):
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
import sys
import time
from datetime import datetime, timedelta, timezone

import akshare as ak
import pandas as pd

# ── Clear proxy ──
for var in ["HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
            "HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
            "NO_PROXY", "no_proxy"]:
    os.environ.pop(var, None)

DATA_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "data")
CSV_DAILY_DIR = os.path.join(DATA_DIR, "csv_daily")
OUTPUT_CSV = os.path.join(DATA_DIR, "corporate_actions.csv")
OUTPUT_BIN = os.path.join(DATA_DIR, "corp_actions.bin")

# ── Get all symbols from CSV files ──
def get_all_symbols():
    symbols = set()
    for fname in os.listdir(CSV_DAILY_DIR):
        if not fname.endswith(".csv"):
            continue
        fpath = os.path.join(CSV_DAILY_DIR, fname)
        with open(fpath) as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if header is None:
                continue
            for row in reader:
                if row:
                    symbols.add(row[0])
    return sorted(symbols)


# Build close-price lookup: symbol -> {date_str -> close_price}
def build_price_lookup(symbols):
    price_map = {}
    for fname in os.listdir(CSV_DAILY_DIR):
        if not fname.endswith(".csv"):
            continue
        fpath = os.path.join(CSV_DAILY_DIR, fname)
        with open(fpath) as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if header is None:
                continue
            for row in reader:
                if len(row) < 6:
                    continue
                sym = row[0]
                date_str = row[1]
                try:
                    close = float(row[5])
                except (ValueError, IndexError):
                    continue
                if sym not in price_map:
                    price_map[sym] = {}
                price_map[sym][date_str] = close
    return price_map


def date_to_epoch_us(date_str: str) -> int:
    """YYYY-MM-DD -> epoch microseconds"""
    dt = datetime.strptime(date_str, "%Y-%m-%d")
    return int(dt.replace(tzinfo=timezone.utc).timestamp() * 1_000_000)


def epoch_us_to_date(ts_us: int) -> str:
    dt = datetime.fromtimestamp(ts_us / 1_000_000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d")


def strip_market_suffix(symbol: str) -> str:
    """000001.SZ -> 000001"""
    return symbol.split(".")[0]


def compute_adjust_factor(dividend_per_share: float, close_price: float) -> float:
    """
    Backward adjustment factor for a cash dividend.
    adjust_factor = close / (close - dividend)
    So backward-adjusted price = raw_price / adjust_factor
                              = raw_price * (close - dividend) / close
    """
    if close_price <= 0 or dividend_per_share <= 0:
        return 1.0
    adj = close_price / (close_price - dividend_per_share)
    return max(1.0, min(adj, 1.5))  # sanity clamp


# ── Main ──
def main():
    print("[1/4] Reading symbols from CSV files...")
    symbols = get_all_symbols()
    print(f"  Found {len(symbols)} unique symbols")

    print("[1b/4] Building close-price lookup...")
    price_lookup = build_price_lookup(symbols)
    print(f"  Price data for {len(price_lookup)} symbols")

    print("[2/4] Downloading dividend data via AKShare...")
    all_actions = []  # (symbol, date_us, type, value, adjust_factor)

    total = len(symbols)
    for idx, sym in enumerate(symbols, 1):
        code = strip_market_suffix(sym)
        try:
            df = ak.stock_history_dividend_detail(symbol=code, indicator="分红")
        except Exception as e:
            if idx % 50 == 0 or idx == 1 or idx == total:
                print(f"  [{idx}/{total}] {sym}: error: {e}")
            time.sleep(0.3)
            continue

        if df.empty:
            if idx % 50 == 0 or idx == total:
                print(f"  [{idx}/{total}] {sym}: no dividend data")
            time.sleep(0.3)
            continue

        # Filter executed dividends (进度 == '实施') with ex-dividend date
        div_df = df[df["进度"] == "实施"].copy()
        if div_df.empty:
            if idx % 50 == 0 or idx == total:
                print(f"  [{idx}/{total}] {sym}: no executed dividends")
            time.sleep(0.3)
            continue

        for _, row in div_df.iterrows():
            ex_div_date = row.get("除权除息日")
            if pd.isna(ex_div_date) or not ex_div_date:
                continue
            date_str = str(ex_div_date)[:10]

            # 派息 is per 10 shares
            div_per_10 = float(row.get("派息", 0))
            if div_per_10 <= 0:
                continue
            div_per_share = div_per_10 / 10.0

            # Close price on ex-dividend date for adjust_factor
            close_price = 0
            if sym in price_lookup and date_str in price_lookup[sym]:
                close_price = price_lookup[sym][date_str]
            else:
                # Try previous day
                dt = datetime.strptime(date_str, "%Y-%m-%d")
                for offset in range(1, 10):
                    prev = (dt - timedelta(days=offset)).strftime("%Y-%m-%d")
                    if sym in price_lookup and prev in price_lookup[sym]:
                        close_price = price_lookup[sym][prev]
                        break

            adj_factor = compute_adjust_factor(div_per_share, close_price) if close_price > 0 else 1.0
            date_us = date_to_epoch_us(date_str)

            all_actions.append((sym, date_us, 0, div_per_share, adj_factor))

        if idx % 50 == 0 or idx == 1 or idx == total:
            print(f"  [{idx}/{total}] {sym}: {len(div_df)} dividends")
        time.sleep(0.3)

    print(f"\n  Total dividend actions: {len(all_actions)}")

    # ── Step 3: Write CSV ──
    print("[3/4] Writing CSV...")
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["symbol", "date", "type", "value", "adjust_factor"])
        for sym, date_us, atype, val, adj in sorted(all_actions, key=lambda x: (x[0], x[1])):
            date_str = epoch_us_to_date(date_us)
            type_str = ["dividend", "split", "rights", "suspension"][atype]
            writer.writerow([sym, date_str, type_str, val, adj])
    print(f"  CSV: {OUTPUT_CSV}")

    # ── Step 4: Write binary ──
    print("[4/4] Writing binary...")
    magic = 0x434F5250
    version = 1
    num_actions = len(all_actions)
    header_size = 64

    with open(OUTPUT_BIN, "wb") as f:
        # Header
        f.write(struct.pack("<II", magic, version))
        f.write(struct.pack("<I", num_actions))
        f.write(b"\x00" * 52)  # reserved padding

        # Sorted by (symbol, date)
        for sym, date_us, atype, val, adj in sorted(all_actions, key=lambda x: (x[0], x[1])):
            sym_bytes = sym.encode("utf-8")
            slen = len(sym_bytes)
            f.write(struct.pack("<H", slen))
            f.write(sym_bytes)
            f.write(struct.pack("<q", date_us))
            f.write(struct.pack("<B", atype))
            f.write(struct.pack("<d", val))
            f.write(struct.pack("<d", adj))

    print(f"  Binary: {OUTPUT_BIN} ({os.path.getsize(OUTPUT_BIN)} bytes, {num_actions} actions)")
    print(f"\nDone.")


if __name__ == "__main__":
    main()
