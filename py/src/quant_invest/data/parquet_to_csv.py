"""parquet_to_csv.py — Convert Parquet daily bar files to CSV for C++ DataInitializer.

Reads Parquet files from py/data/daily/*.parquet, applies column mapping,
adds symbol column, normalizes date format, and writes CSV output.

Usage:
    python -m quant_invest.data.parquet_to_csv --input py/data/daily --output data/csv_daily
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import pandas as pd


# ── Column mapping: Parquet column → CSV column ──
COLUMN_MAP: dict[str, str] = {
    "trade_date": "date",
    "open": "open",
    "high": "high",
    "low": "low",
    "close": "close",
    "volume": "volume",
    "amount": "amount",
}

# Columns to keep in the output CSV (in this order)
OUTPUT_COLUMNS = ["symbol", "date", "open", "high", "low", "close", "volume", "amount"]

# Pattern to extract symbol from filename: 600519_SH.parquet → 600519.SH
_FILENAME_RE = re.compile(r"^([0-9]{6})_(SH|SZ)\.parquet$")


def filename_to_symbol(filename: str) -> str:
    """Convert Parquet filename to symbol format.

    Examples:
        600519_SH.parquet → 600519.SH
        000001_SZ.parquet → 000001.SZ
    """
    m = _FILENAME_RE.match(filename)
    if not m:
        raise ValueError(f"Cannot parse symbol from filename: {filename!r}")
    code, exchange = m.group(1), m.group(2)
    return f"{code}.{exchange}"


def convert_single_file(input_path: Path, output_path: Path) -> pd.DataFrame:
    """Convert a single Parquet file to CSV.

    Args:
        input_path: Path to the .parquet file.
        output_path: Path to write the .csv file.

    Returns:
        The converted DataFrame (for testing).
    """
    # Read Parquet
    df = pd.read_parquet(input_path)

    # trade_date is the index — reset to a column
    if df.index.name == "trade_date":
        df = df.reset_index()

    # Rename columns per mapping
    df = df.rename(columns=COLUMN_MAP)

    # Extract symbol from filename and add as first column
    symbol = filename_to_symbol(input_path.name)
    df["symbol"] = symbol

    # Normalize date format to YYYY-MM-DD string
    if "date" in df.columns:
        df["date"] = pd.to_datetime(df["date"]).dt.strftime("%Y-%m-%d")

    # Select and order output columns
    available = [c for c in OUTPUT_COLUMNS if c in df.columns]
    df = df[available]

    # Write CSV — keep float precision for prices (C++ side does ×10000)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(output_path, index=False)

    return df


def convert_directory(input_dir: Path, output_dir: Path) -> list[Path]:
    """Convert all Parquet files in a directory to CSV.

    Args:
        input_dir: Directory containing .parquet files.
        output_dir: Directory to write .csv files.

    Returns:
        List of output CSV paths.
    """
    output_dir.mkdir(parents=True, exist_ok=True)

    parquet_files = sorted(input_dir.glob("*.parquet"))
    if not parquet_files:
        print(f"No .parquet files found in {input_dir}", file=sys.stderr)
        return []

    output_paths: list[Path] = []
    for pq_path in parquet_files:
        csv_name = pq_path.stem + ".csv"  # 600519_SH.csv
        csv_path = output_dir / csv_name
        convert_single_file(pq_path, csv_path)
        output_paths.append(csv_path)
        print(f"  {pq_path.name} → {csv_path.name}")

    return output_paths


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert Parquet daily bar files to CSV for C++ DataInitializer"
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=Path("py/data/daily"),
        help="Input directory containing .parquet files (default: py/data/daily)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("data/csv_daily"),
        help="Output directory for .csv files (default: data/csv_daily)",
    )
    args = parser.parse_args()

    input_dir: Path = args.input
    output_dir: Path = args.output

    if not input_dir.is_dir():
        print(f"Input directory not found: {input_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Converting Parquet → CSV: {input_dir} → {output_dir}")
    output_paths = convert_directory(input_dir, output_dir)
    print(f"Done: {len(output_paths)} file(s) converted")


if __name__ == "__main__":
    main()
