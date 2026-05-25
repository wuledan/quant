#!/usr/bin/env python3
"""Download ALL A-share ETF daily kline data via AKShare and update symbol_names.json.

Data source: Sina finance API (fund_etf_hist_sina).
ETF list: Eastmoney realtime snapshot (fund_etf_spot_em).
"""

import json
import os
import sys
import time

import akshare as ak

CSV_DIR = os.path.join(os.path.dirname(__file__), "..", "data", "csv_daily")
NAMES_PATH = os.path.join(os.path.dirname(__file__), "..", "data", "symbol_names.json")


def code_to_symbol(code: str) -> str:
    """Convert numeric code to full symbol with exchange suffix.
    15xxxx → .SZ (Shenzhen)
    51/52/53/56/58xxxx → .SH (Shanghai)
    """
    return f"{code}.SZ" if code.startswith("15") else f"{code}.SH"


def code_to_filename(code: str) -> str:
    """Convert code to CSV filename (e.g. 510050 → 510050_SH.csv)."""
    suffix = "SZ" if code.startswith("15") else "SH"
    return f"{code}_{suffix}.csv"


def code_to_sina_prefix(code: str) -> str:
    """Convert code to Sina API prefix (e.g. 512880 → sh512880)."""
    prefix = "sz" if code.startswith("15") else "sh"
    return f"{prefix}{code}"


def download_etf(code: str, name: str) -> bool:
    """Download one ETF's daily data via Sina API. Returns True on success."""
    symbol = code_to_symbol(code)
    filename = code_to_filename(code)
    filepath = os.path.join(CSV_DIR, filename)

    # Skip if already exists
    if os.path.exists(filepath):
        return True

    sina_symbol = code_to_sina_prefix(code)
    try:
        df = ak.fund_etf_hist_sina(symbol=sina_symbol)
    except Exception as e:
        print(f"  FAIL {symbol} ({name}): {e}", file=sys.stderr)
        return False

    if df is None or df.empty:
        # Some ETFs have no trading history (just listed, or delisted) — skip silently
        return False

    # Sina returns columns: date, open, high, low, close, volume, amount
    df.insert(0, "symbol", symbol)
    df = df[["symbol", "date", "open", "high", "low", "close", "volume", "amount"]]
    df = df.sort_values("date")
    df.to_csv(filepath, index=False)
    return True


def main():
    os.makedirs(CSV_DIR, exist_ok=True)

    # Load existing symbol names
    with open(NAMES_PATH, "r", encoding="utf-8") as f:
        names = json.load(f)

    # Get ALL ETF codes and names from eastmoney
    print("[ETF Download] Fetching ETF list from eastmoney...")
    df_all = ak.fund_etf_spot_em()
    codes = df_all["代码"].astype(str).tolist()
    etf_names = df_all["名称"].tolist()
    total = len(codes)
    print(f"[ETF Download] Total ETFs in market: {total}")

    # Also check existing files count
    existing = 0
    for code in codes:
        filename = code_to_filename(code)
        if os.path.exists(os.path.join(CSV_DIR, filename)):
            existing += 1
    print(f"[ETF Download] Already downloaded: {existing}")

    success = 0
    skipped = 0
    failed = 0
    empty = 0
    for i, (code, name) in enumerate(zip(codes, etf_names)):
        symbol = code_to_symbol(code)
        filename = code_to_filename(code)
        filepath = os.path.join(CSV_DIR, filename)

        # Skip existing
        if os.path.exists(filepath):
            skipped += 1
            continue

        ok = download_etf(code, name)
        if ok:
            success += 1
            names[str(code)] = name  # update symbol name mapping
        else:
            # Check if file was created despite apparent failure
            if os.path.exists(filepath) and os.path.getsize(filepath) > 0:
                success += 1
                names[str(code)] = name
            else:
                failed += 1

        # Progress every 50
        if (i + 1) % 50 == 0:
            pct = (i + 1) / total * 100
            print(f"  Progress: {i+1}/{total} ({pct:.0f}%) — OK={success} SKIP={skipped} FAIL={failed} EMPTY={empty}")

        time.sleep(0.3)  # be gentle to Sina API

    # Write updated names
    with open(NAMES_PATH, "w", encoding="utf-8") as f:
        json.dump(names, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(f"\n=== ETF Download Complete ===")
    print(f"  Total in market: {total}")
    print(f"  Newly downloaded: {success}")
    print(f"  Already existed: {skipped}")
    print(f"  Failed: {failed}")
    print(f"  Symbol names updated: {len(names)} total entries")


if __name__ == "__main__":
    main()
