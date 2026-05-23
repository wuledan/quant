#!/usr/bin/env python3
"""
Download corporate actions (dividends + splits) for all A-share stocks via AKShare.

Uses ak.stock_fhps_detail_em() — EastMoney dividend/split detail API.
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
    double value         — dividend per share (yuan) or split ratio
    double adjust_factor — backward price adjustment factor
"""
import csv
import os
import struct
import time
from datetime import datetime, timedelta, timezone

import akshare as ak
import pandas as pd

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


def compute_adjust_factor(div_per_share: float, close_price: float) -> float:
    """Backward adjustment factor for cash dividend."""
    if close_price <= 0 or div_per_share <= 0:
        return 1.0
    adj = close_price / (close_price - div_per_share)
    return max(1.0, min(adj, 1.5))


# ── Main ──
def main():
    # ── Step 1: Read CSVs once for symbols + price lookup ──
    print("[1/3] Reading CSV files (single pass)...", flush=True)
    symbols = set()
    price_lookup = {}

    csv_files = sorted(f for f in os.listdir(CSV_DAILY_DIR) if f.endswith(".csv"))
    total_files = len(csv_files)
    for idx, fname in enumerate(csv_files, 1):
        fpath = os.path.join(CSV_DAILY_DIR, fname)
        with open(fpath, newline="") as f:
            reader = csv.reader(f)
            try:
                next(reader)  # skip header
            except StopIteration:
                continue
            for row in reader:
                if len(row) < 6:
                    continue
                sym = row[0]
                symbols.add(sym)
                try:
                    price_lookup.setdefault(sym, {})[row[1]] = float(row[5])
                except (ValueError, IndexError):
                    continue
        if idx % 100 == 0 or idx == total_files:
            print(f"  ... {idx}/{total_files} files", flush=True)

    symbols = sorted(symbols)
    print(f"  {len(symbols)} symbols, {sum(len(v) for v in price_lookup.values())} price records", flush=True)

    # ── Step 2: Download via EastMoney API ──
    print("[2/3] Downloading dividend/split data via EastMoney...", flush=True)
    all_actions = []

    total = len(symbols)
    for idx, sym in enumerate(symbols, 1):
        code = strip_market_suffix(sym)
        try:
            df = ak.stock_fhps_detail_em(symbol=code)
        except Exception as e:
            if idx % 50 == 0 or idx == 1:
                print(f"  [{idx}/{total}] {sym}: error: {e}", flush=True)
            time.sleep(0.2)
            continue

        if df.empty:
            if idx % 50 == 0 or idx == 1:
                print(f"  [{idx}/{total}] {sym}: no data", flush=True)
            time.sleep(0.2)
            continue

        # Keep only executed actions
        # 方案进度: '实施分配' = executed
        if "方案进度" in df.columns:
            df = df[df["方案进度"].str.contains("实施", na=False)].copy()

        count = 0
        for _, row in df.iterrows():
            # Cash dividend per 10 shares: '现金分红-现金分红比例'
            div_col = "现金分红-现金分红比例"
            div_per_10 = float(row.get(div_col, 0)) if div_col in row.index else 0
            if div_per_10 > 0 and not pd.isna(div_per_10):
                div_per_share = div_per_10 / 10.0

                ex_div = row.get("除权除息日", None)
                if pd.isna(ex_div) or not ex_div:
                    continue
                date_str = str(ex_div)[:10]

                # Close price for adjust factor
                close_price = 0
                price_map = price_lookup.get(sym, {})
                if date_str in price_map:
                    close_price = price_map[date_str]
                else:
                    dt = datetime.strptime(date_str, "%Y-%m-%d")
                    for off in range(1, 10):
                        prev = (dt - timedelta(days=off)).strftime("%Y-%m-%d")
                        if prev in price_map:
                            close_price = price_map[prev]
                            break

                adj = compute_adjust_factor(div_per_share, close_price) if close_price > 0 else 1.0
                all_actions.append((sym, date_to_epoch_us(date_str), 0, div_per_share, adj))
                count += 1

            # Stock split: '送转股份-送转总比例' (per 10 shares)
            split_col = "送转股份-送转总比例"
            split_val = float(row.get(split_col, 0)) if split_col in row.index else 0
            if split_val > 0 and not pd.isna(split_val):
                ex_div = row.get("除权除息日", None)
                if pd.isna(ex_div) or not ex_div:
                    continue
                date_str = str(ex_div)[:10]
                split_ratio = split_val / 10.0  # 10送X → ratio per share
                # adjust_factor for split: 1 / (1 + split_ratio)
                adj = 1.0 / (1.0 + split_ratio) if split_ratio > 0 else 1.0
                all_actions.append((sym, date_to_epoch_us(date_str), 1, split_ratio, adj))
                count += 1

            if count > 0 and idx % 50 == 0:
                print(f"  [{idx}/{total}] {sym}: {count} actions", flush=True)

        time.sleep(0.2)  # rate limit

    print(f"\n  Total actions: {len(all_actions)}", flush=True)

    # ── Step 3: Write outputs ──
    print("[3/3] Writing output files...", flush=True)

    # CSV
    sorted_actions = sorted(all_actions, key=lambda x: (x[0], x[1]))
    with open(OUTPUT_CSV, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["symbol", "date", "type", "value", "adjust_factor"])
        for sym, date_us, atype, val, adj in sorted_actions:
            writer.writerow([sym, epoch_us_to_date(date_us),
                            ["dividend", "split", "rights", "suspension"][atype], val, adj])
    print(f"  CSV: {OUTPUT_CSV}")

    # Binary
    with open(OUTPUT_BIN, "wb") as f:
        f.write(struct.pack("<II", 0x434F5250, 1))
        f.write(struct.pack("<I", len(all_actions)))
        f.write(b"\x00" * 52)
        for sym, date_us, atype, val, adj in sorted_actions:
            sb = sym.encode("utf-8")
            f.write(struct.pack("<H", len(sb)))
            f.write(sb)
            f.write(struct.pack("<q", date_us))
            f.write(struct.pack("<B", atype))
            f.write(struct.pack("<d", val))
            f.write(struct.pack("<d", adj))
    print(f"  Binary: {OUTPUT_BIN} ({os.path.getsize(OUTPUT_BIN)} bytes, {len(all_actions)} actions)")
    print("\nDone.", flush=True)


if __name__ == "__main__":
    main()
