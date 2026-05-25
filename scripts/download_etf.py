#!/usr/bin/env python3
"""Download ETF daily kline data via AKShare and update symbol_names.json."""

import json
import os
import sys
import time

import akshare as ak

CSV_DIR = os.path.join(os.path.dirname(__file__), "..", "data", "csv_daily")
NAMES_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "symbol_names.json")

# ETF list with Chinese names
ETF_LIST = [
    ("510050", "上证50ETF"),
    ("510300", "沪深300ETF"),
    ("510500", "中证500ETF"),
    ("159915", "创业板ETF"),
    ("159919", "沪深300ETF易方达"),
    ("159922", "中证500ETF易方达"),
    ("588000", "科创50ETF"),
    ("588080", "科创板50ETF"),
    ("159845", "中证1000ETF"),
    ("159949", "创业板50ETF"),
    ("512880", "证券ETF"),
    ("512800", "银行ETF"),
    ("512100", "中证1000ETF"),
    ("510880", "红利ETF"),
    ("512010", "医药ETF"),
    ("512070", "非银ETF"),
    ("512690", "酒ETF"),
    ("511010", "国债ETF"),
    ("511260", "十年国债ETF"),
    ("518880", "黄金ETF"),
    ("515790", "光伏ETF"),
    ("512480", "半导体ETF"),
    ("159995", "芯片ETF"),
    ("515050", "5GETF"),
    ("516160", "新能源ETF"),
    ("515030", "新能源车ETF"),
    ("159996", "家电ETF"),
]


def code_to_symbol(code: str) -> str:
    """Convert numeric code to full symbol with exchange suffix.
    Codes starting with 51/58 → .SH, others → .SZ
    """
    return f"{code}.SH" if code.startswith(("51", "58")) else f"{code}.SZ"


def code_to_filename(code: str) -> str:
    """Convert code to CSV filename (e.g. 510050 → 510050_SH.csv)."""
    suffix = "SH" if code.startswith(("51", "58")) else "SZ"
    return f"{code}_{suffix}.csv"


def code_to_sina_prefix(code: str) -> str:
    """Convert code to Sina API prefix (e.g. 512880 → sh512880)."""
    prefix = "sh" if code.startswith(("51", "58")) else "sz"
    return f"{prefix}{code}"


def download_etf(code: str, name: str) -> bool:
    """Download one ETF's daily data via Sina API. Returns True on success."""
    symbol = code_to_symbol(code)
    filename = code_to_filename(code)
    filepath = os.path.join(CSV_DIR, filename)

    # Skip if already exists
    if os.path.exists(filepath):
        print(f"  SKIP {symbol} ({name}) — already exists")
        return True

    sina_symbol = code_to_sina_prefix(code)
    try:
        df = ak.fund_etf_hist_sina(symbol=sina_symbol)
    except Exception as e:
        print(f"  FAIL {symbol} ({name}): {e}", file=sys.stderr)
        return False

    if df is None or df.empty:
        print(f"  FAIL {symbol} ({name}): empty data")
        return False

    # Sina returns columns: date, open, high, low, close, volume, amount
    # Insert symbol column
    df.insert(0, "symbol", symbol)
    df = df[["symbol", "date", "open", "high", "low", "close", "volume", "amount"]]

    # Sort by date ascending
    df = df.sort_values("date")

    df.to_csv(filepath, index=False)
    print(f"  OK   {symbol} ({name}) — {len(df)} rows")
    return True


def main():
    os.makedirs(CSV_DIR, exist_ok=True)

    # Load existing symbol names
    with open(NAMES_PATH, "r", encoding="utf-8") as f:
        names = json.load(f)

    success = 0
    for code, name in ETF_LIST:
        ok = download_etf(code, name)
        if ok:
            success += 1
            # Add/update name mapping (store without exchange suffix, like stocks)
            # ETF codes are 6 digits, no conflict with stock codes
            names[code] = name
        time.sleep(0.5)  # be gentle to AKShare API

    # Write updated names
    with open(NAMES_PATH, "w", encoding="utf-8") as f:
        json.dump(names, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(f"\nDone. {success}/{len(ETF_LIST)} ETFs downloaded. symbol_names.json updated.")


if __name__ == "__main__":
    main()
