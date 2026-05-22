"""test_parquet_to_csv.py — Tests for Parquet-to-CSV converter.

Tests cover:
  - filename_to_symbol: filename parsing and error handling
  - convert_single_file: column mapping, date format, symbol injection
  - convert_directory: batch conversion, empty directory
  - CSV output format: header order, data integrity, float precision
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import pandas as pd
import pytest

from quant_invest.data.parquet_to_csv import (
    OUTPUT_COLUMNS,
    convert_directory,
    convert_single_file,
    filename_to_symbol,
)


# ── filename_to_symbol ──

class TestFilenameToSymbol:
    def test_sh_exchange(self) -> None:
        assert filename_to_symbol("600519_SH.parquet") == "600519.SH"

    def test_sz_exchange(self) -> None:
        assert filename_to_symbol("000001_SZ.parquet") == "000001.SZ"

    def test_invalid_format_no_underscore(self) -> None:
        with pytest.raises(ValueError, match="Cannot parse symbol"):
            filename_to_symbol("600519.parquet")

    def test_invalid_format_wrong_code_length(self) -> None:
        with pytest.raises(ValueError, match="Cannot parse symbol"):
            filename_to_symbol("6019_SH.parquet")

    def test_invalid_format_wrong_exchange(self) -> None:
        with pytest.raises(ValueError, match="Cannot parse symbol"):
            filename_to_symbol("600519_XX.parquet")

    def test_invalid_format_not_parquet(self) -> None:
        with pytest.raises(ValueError, match="Cannot parse symbol"):
            filename_to_symbol("600519_SH.csv")


# ── convert_single_file ──

class TestConvertSingleFile:
    @pytest.fixture()
    def sample_parquet(self, tmp_path: Path) -> Path:
        """Create a sample Parquet file mimicking real data."""
        df = pd.DataFrame({
            "trade_date": pd.to_datetime(["2024-01-02", "2024-01-03", "2024-01-04"]),
            "open":   [1680.01, 1690.50, 1685.00],
            "high":   [1695.00, 1700.00, 1690.00],
            "low":    [1675.00, 1685.00, 1680.00],
            "close":  [1690.00, 1695.50, 1688.00],
            "volume": [1000000, 1200000, 950000],
            "amount": [1.69e10, 2.04e10, 1.60e10],
        })
        df = df.set_index("trade_date")
        pq_path = tmp_path / "600519_SH.parquet"
        df.to_parquet(pq_path)
        return pq_path

    def test_column_mapping(self, sample_parquet: Path, tmp_path: Path) -> None:
        """trade_date → date, other columns preserved."""
        csv_path = tmp_path / "600519_SH.csv"
        result = convert_single_file(sample_parquet, csv_path)

        assert "date" in result.columns
        assert "trade_date" not in result.columns

    def test_symbol_column_added(self, sample_parquet: Path, tmp_path: Path) -> None:
        """Symbol extracted from filename and added as first column."""
        csv_path = tmp_path / "600519_SH.csv"
        result = convert_single_file(sample_parquet, csv_path)

        assert "symbol" in result.columns
        assert result["symbol"].iloc[0] == "600519.SH"

    def test_date_format_yyyy_mm_dd(self, sample_parquet: Path, tmp_path: Path) -> None:
        """Dates formatted as YYYY-MM-DD strings."""
        csv_path = tmp_path / "600519_SH.csv"
        result = convert_single_file(sample_parquet, csv_path)

        assert result["date"].iloc[0] == "2024-01-02"
        assert result["date"].iloc[1] == "2024-01-03"

    def test_output_column_order(self, sample_parquet: Path, tmp_path: Path) -> None:
        """Output columns follow OUTPUT_COLUMNS order."""
        csv_path = tmp_path / "600519_SH.csv"
        result = convert_single_file(sample_parquet, csv_path)

        expected = [c for c in OUTPUT_COLUMNS if c in result.columns]
        assert list(result.columns) == expected

    def test_csv_file_written(self, sample_parquet: Path, tmp_path: Path) -> None:
        """CSV file is physically written and readable."""
        csv_path = tmp_path / "600519_SH.csv"
        convert_single_file(sample_parquet, csv_path)

        assert csv_path.exists()
        df_read = pd.read_csv(csv_path)
        assert len(df_read) == 3
        assert "symbol" in df_read.columns

    def test_float_precision_preserved(self, tmp_path: Path) -> None:
        """Float values (prices) are not truncated in CSV output."""
        df = pd.DataFrame({
            "trade_date": pd.to_datetime(["2024-01-02"]),
            "open":   [1680.01],
            "high":   [1695.99],
            "low":    [1675.50],
            "close":  [1690.00],
            "volume": [1000000],
            "amount": [1.69e10],
        })
        df = df.set_index("trade_date")
        pq_path = tmp_path / "600519_SH.parquet"
        df.to_parquet(pq_path)

        csv_path = tmp_path / "600519_SH.csv"
        convert_single_file(pq_path, csv_path)

        df_read = pd.read_csv(csv_path)
        assert abs(df_read["open"].iloc[0] - 1680.01) < 0.01
        assert abs(df_read["high"].iloc[0] - 1695.99) < 0.01

    def test_creates_output_directory(self, sample_parquet: Path, tmp_path: Path) -> None:
        """Output directory is created if it doesn't exist."""
        csv_path = tmp_path / "subdir" / "nested" / "600519_SH.csv"
        convert_single_file(sample_parquet, csv_path)
        assert csv_path.exists()

    def test_row_count_preserved(self, sample_parquet: Path, tmp_path: Path) -> None:
        """Number of rows matches the source Parquet."""
        csv_path = tmp_path / "600519_SH.csv"
        result = convert_single_file(sample_parquet, csv_path)
        assert len(result) == 3


# ── convert_directory ──

class TestConvertDirectory:
    @pytest.fixture()
    def parquet_dir(self, tmp_path: Path) -> Path:
        """Create a directory with multiple Parquet files."""
        input_dir = tmp_path / "input"
        input_dir.mkdir()

        for symbol in ["600519_SH", "000001_SZ", "300750_SZ"]:
            code, exchange = symbol.split("_")
            df = pd.DataFrame({
                "trade_date": pd.to_datetime(["2024-01-02", "2024-01-03"]),
                "open":   [100.0, 101.0],
                "high":   [102.0, 103.0],
                "low":    [99.0, 100.0],
                "close":  [101.0, 102.0],
                "volume": [500000, 600000],
                "amount": [5.05e9, 6.12e9],
            })
            df = df.set_index("trade_date")
            df.to_parquet(input_dir / f"{symbol}.parquet")

        return input_dir

    def test_batch_conversion(self, parquet_dir: Path, tmp_path: Path) -> None:
        """All Parquet files in directory are converted."""
        output_dir = tmp_path / "output"
        paths = convert_directory(parquet_dir, output_dir)

        assert len(paths) == 3
        for p in paths:
            assert p.exists()
            assert p.suffix == ".csv"

    def test_output_naming(self, parquet_dir: Path, tmp_path: Path) -> None:
        """Output CSV filenames match Parquet stem."""
        output_dir = tmp_path / "output"
        paths = convert_directory(parquet_dir, output_dir)

        names = {p.name for p in paths}
        assert "600519_SH.csv" in names
        assert "000001_SZ.csv" in names
        assert "300750_SZ.csv" in names

    def test_empty_directory(self, tmp_path: Path) -> None:
        """Empty input directory returns empty list."""
        input_dir = tmp_path / "empty"
        input_dir.mkdir()
        output_dir = tmp_path / "output"

        paths = convert_directory(input_dir, output_dir)
        assert paths == []

    def test_creates_output_dir(self, parquet_dir: Path, tmp_path: Path) -> None:
        """Output directory is created if it doesn't exist."""
        output_dir = tmp_path / "new" / "output"
        convert_directory(parquet_dir, output_dir)
        assert output_dir.is_dir()


# ── CSV format integrity ──

class TestCsvFormatIntegrity:
    def test_csv_header_matches_output_columns(self, tmp_path: Path) -> None:
        """CSV header line matches OUTPUT_COLUMNS exactly."""
        df = pd.DataFrame({
            "trade_date": pd.to_datetime(["2024-01-02"]),
            "open":   [100.0],
            "high":   [102.0],
            "low":    [99.0],
            "close":  [101.0],
            "volume": [500000],
            "amount": [5.0e9],
        })
        df = df.set_index("trade_date")
        pq_path = tmp_path / "600519_SH.parquet"
        df.to_parquet(pq_path)

        csv_path = tmp_path / "600519_SH.csv"
        convert_single_file(pq_path, csv_path)

        header_line = csv_path.read_text().split("\n")[0].strip()
        assert header_line == ",".join(OUTPUT_COLUMNS)

    def test_data_round_trip(self, tmp_path: Path) -> None:
        """Data values survive Parquet → CSV round-trip."""
        df = pd.DataFrame({
            "trade_date": pd.to_datetime(["2024-01-02", "2024-01-03"]),
            "open":   [1680.01, 1690.50],
            "high":   [1695.00, 1700.00],
            "low":    [1675.00, 1685.00],
            "close":  [1690.00, 1695.50],
            "volume": [1000000, 1200000],
            "amount": [1.69e10, 2.04e10],
        })
        df = df.set_index("trade_date")
        pq_path = tmp_path / "600519_SH.parquet"
        df.to_parquet(pq_path)

        csv_path = tmp_path / "600519_SH.csv"
        convert_single_file(pq_path, csv_path)

        df_csv = pd.read_csv(csv_path)

        # Check numeric values match (within float precision)
        for col in ["open", "high", "low", "close"]:
            for i in range(2):
                assert abs(df_csv[col].iloc[i] - df[col].iloc[i]) < 0.01, \
                    f"Mismatch in {col}[{i}]: {df_csv[col].iloc[i]} vs {df[col].iloc[i]}"

        # Volume is integer — exact match
        assert list(df_csv["volume"]) == list(df["volume"])
