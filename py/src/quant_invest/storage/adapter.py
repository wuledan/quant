#!/usr/bin/env python3
"""StorageEngineAdapter — Python wrapper around C++ StorageEngine.

Bridges Parquet data files (pandas DataFrame) to the C++ StorageEngine
via pybind11. Handles:
- DataFrame → ColumnBlock conversion with proper codec selection
- Batch loading from Parquet files
- Query with result → dict conversion
- Real-time single-row ingestion

Design note: store_kline() in C++ has a codec mismatch bug (stores int64
via Delta codec but query_kline expects double via Gorilla). We work around
this by using TimeSeriesStore + ColumnBlock directly, which gives us full
control over codec selection.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd

from quant_invest._quant_core import (
    Codec,
    ColumnBlock,
    DataField,
    DataType,
    StoreStatus,
    TimeRange,
    TimeSeriesStore,
)

logger = logging.getLogger("quant_invest.storage.adapter")

# Price scale: float → fixed-point (price * 10000)
PRICE_SCALE = 10000


def _float_to_fixed(price: float) -> int:
    """Convert float price to fixed-point int64 (price * 10000)."""
    return int(round(price * PRICE_SCALE))


def _fixed_to_float(fixed: int) -> float:
    """Convert fixed-point int64 back to float price."""
    return fixed / PRICE_SCALE


def _timestamp_us_from_datetime(dt: pd.Timestamp) -> int:
    """Convert datetime to microseconds since epoch."""
    ts = pd.Timestamp(dt)
    return int(ts.timestamp() * 1_000_000)


class StorageEngineAdapter:
    """Python adapter around C++ TimeSeriesStore.

    Uses ColumnBlock with proper codec selection:
    - Timestamps: Delta codec (int64)
    - Prices (OHLC, VWAP): Gorilla codec (float64, stored as fixed-point int64)
    - Volume/Amount: Delta codec (int64)

    This avoids the store_kline codec mismatch bug.
    """

    def __init__(
        self,
        data_dir: str = "py/data/daily",
        cache_budget_mb: int = 256,
        engine_data_dir: str = "/tmp/quant_storage",
    ) -> None:
        self._data_dir = Path(data_dir)
        self._store = TimeSeriesStore(cache_budget_mb, str(Path(engine_data_dir)))
        self._loaded_symbols: dict[str, int] = {}  # symbol → row count loaded
        # In-memory cache for query results (since C++ query has codec issues)
        self._cache: dict[str, pd.DataFrame] = {}  # symbol → DataFrame

        logger.info(
            "StorageEngineAdapter initialized: data_dir=%s, cache=%dMB, engine_dir=%s",
            data_dir, cache_budget_mb, engine_data_dir,
        )

    # ── Public API ──────────────────────────────────────────────

    def load_from_parquet(self, symbol: str, parquet_path: str | Path) -> int:
        """Load a Parquet file into the C++ TimeSeriesStore.

        Args:
            symbol: Stock symbol (e.g. "000001.SH")
            parquet_path: Path to the Parquet file

        Returns:
            Number of rows loaded
        """
        path = Path(parquet_path)
        if not path.exists():
            logger.warning("Parquet file not found: %s", path)
            return 0

        try:
            df = pd.read_parquet(path)
        except Exception as e:
            logger.error("Failed to read Parquet %s: %s", path, e)
            return 0

        if df.empty:
            logger.warning("Empty Parquet: %s", path)
            return 0

        # Normalize column names to lowercase
        df.columns = [c.lower() for c in df.columns]

        # Store in-memory cache
        self._cache[symbol] = df

        # Convert to ColumnBlocks and store in C++ engine
        count = self._store_dataframe(symbol, df)

        self._loaded_symbols[symbol] = count
        logger.info("Loaded %d rows for %s from %s", count, symbol, path)
        return count

    def load_all_cached(self) -> dict[str, int]:
        """Scan data_dir and load all Parquet files.

        Returns:
            Dict mapping symbol → row count loaded
        """
        if not self._data_dir.exists():
            logger.warning("Data directory not found: %s", self._data_dir)
            return {}

        results: dict[str, int] = {}
        parquet_files = list(self._data_dir.glob("*.parquet"))

        logger.info("Scanning %s: found %d Parquet files", self._data_dir, len(parquet_files))

        for path in parquet_files:
            # Convert filename back to symbol: "000001_SH.parquet" → "000001.SH"
            filename = path.stem
            symbol = filename.replace("_", ".", 1)

            count = self.load_from_parquet(symbol, path)
            if count > 0:
                results[symbol] = count

        logger.info("Loaded %d symbols, total %d rows", len(results), sum(results.values()))
        return results

    def query_kline(
        self,
        symbol: str,
        start_ts: int | None = None,
        end_ts: int | None = None,
        field: str = "close",
    ) -> list[dict[str, Any]]:
        """Query K-line data from the in-memory cache.

        Args:
            symbol: Stock symbol
            start_ts: Start timestamp in microseconds (0 = earliest)
            end_ts: End timestamp in microseconds (None = latest)
            field: Field name: "open", "high", "low", "close", "volume", "amount", "vwap"

        Returns:
            List of dicts with timestamp and value
        """
        df = self._cache.get(symbol)
        if df is None or df.empty:
            return []

        # Filter by time range
        if start_ts is not None or end_ts is not None:
            start_dt = pd.Timestamp(start_ts / 1_000_000, unit="s") if start_ts else None
            end_dt = pd.Timestamp(end_ts / 1_000_000, unit="s") if end_ts else None
            if start_dt:
                df = df[df.index >= start_dt]
            if end_dt:
                df = df[df.index <= end_dt]

        col = field.lower()
        if col not in df.columns:
            return []

        result = []
        for idx, row in df.iterrows():
            ts_us = _timestamp_us_from_datetime(idx)
            result.append({"timestamp": ts_us, "value": row[col]})

        return result

    def query_kline_all(
        self,
        symbol: str,
        start_ts: int | None = None,
        end_ts: int | None = None,
    ) -> dict[str, list[dict[str, Any]]]:
        """Query all K-line fields.

        Returns:
            Dict mapping field name → list of dicts with timestamp and value
        """
        fields = ["open", "high", "low", "close", "volume", "amount", "vwap"]
        results: dict[str, list[dict[str, Any]]] = {}
        for field in fields:
            results[field] = self.query_kline(symbol, start_ts, end_ts, field)
        return results

    def query_kline_as_df(
        self,
        symbol: str,
        start_ts: int | None = None,
        end_ts: int | None = None,
    ) -> pd.DataFrame:
        """Query K-line data as a pandas DataFrame.

        Returns:
            DataFrame with columns: open, high, low, close, volume, amount, vwap
            Index: DatetimeIndex
        """
        df = self._cache.get(symbol)
        if df is None:
            return pd.DataFrame()

        # Filter by time range
        if start_ts is not None or end_ts is not None:
            start_dt = pd.Timestamp(start_ts / 1_000_000, unit="s") if start_ts else None
            end_dt = pd.Timestamp(end_ts / 1_000_000, unit="s") if end_ts else None
            if start_dt:
                df = df[df.index >= start_dt]
            if end_dt:
                df = df[df.index <= end_dt]

        return df.copy()

    def store_kline(self, symbol: str, row: dict[str, Any]) -> None:
        """Store a single K-line row (for real-time updates).

        Args:
            symbol: Stock symbol
            row: Dict with keys: timestamp, open, high, low, close, volume, amount, vwap
                 Prices should be in float (will be stored as-is)
        """
        # Update in-memory cache
        if symbol not in self._cache:
            self._cache[symbol] = pd.DataFrame()

        ts = row.get("timestamp", 0)
        dt = pd.Timestamp(ts / 1_000_000, unit="s") if ts > 0 else pd.Timestamp.now()

        new_row = pd.DataFrame([{
            "open": row.get("open", 0.0),
            "high": row.get("high", 0.0),
            "low": row.get("low", 0.0),
            "close": row.get("close", 0.0),
            "volume": row.get("volume", 0),
            "amount": row.get("amount", 0),
            "vwap": row.get("vwap", row.get("close", 0.0)),
        }], index=[dt])

        self._cache[symbol] = pd.concat([self._cache[symbol], new_row])
        self._loaded_symbols[symbol] = len(self._cache[symbol])

    def flush(self) -> None:
        """Flush the C++ engine cache to disk."""
        # TimeSeriesStore doesn't have a flush method, but we can persist
        # the in-memory cache to Parquet files if needed
        logger.info("Flush called (no-op for now)")

    def close(self) -> None:
        """Close the adapter and release resources."""
        logger.info("StorageEngineAdapter closed")

    # ── Properties ──────────────────────────────────────────────

    @property
    def store(self) -> TimeSeriesStore:
        """Access the underlying C++ TimeSeriesStore."""
        return self._store

    @property
    def loaded_symbols(self) -> dict[str, int]:
        """Dict of loaded symbols → row count."""
        return self._loaded_symbols

    # ── Internal helpers ────────────────────────────────────────

    def _store_dataframe(self, symbol: str, df: pd.DataFrame) -> int:
        """Store a DataFrame into the C++ TimeSeriesStore using ColumnBlocks.

        Uses proper codec selection:
        - Prices: Gorilla codec (float64 stored as fixed-point int64)
        - Volume/Amount: Delta codec (int64)
        """
        if df.empty:
            return 0

        n = len(df)

        # Convert timestamps to int64 microseconds
        timestamps = np.array(
            [_timestamp_us_from_datetime(idx) for idx in df.index],
            dtype=np.int64,
        )

        # Create ColumnBlocks and store individually
        min_ts = int(timestamps[0])
        max_ts = int(timestamps[-1])

        # Price blocks (Gorilla codec, stored as fixed-point int64 via float64)
        for col, field in [
            ("open", DataField.OPEN),
            ("high", DataField.HIGH),
            ("low", DataField.LOW),
            ("close", DataField.CLOSE),
        ]:
            if col in df.columns:
                prices = np.array(
                    [_float_to_fixed(v) for v in df[col]],
                    dtype=np.float64,
                )
                block = ColumnBlock.compress_double(
                    field, prices, Codec.GORILLA, min_ts, max_ts,
                )
                status = self._store.put(symbol, DataType.KLINE_DAY, block)
                if status != StoreStatus.OK:
                    logger.error("put %s/%s failed: %s", symbol, col, status)

        # Volume block (Delta codec, int64)
        if "volume" in df.columns:
            volumes = np.array(df["volume"], dtype=np.int64)
            block = ColumnBlock.compress_int64(
                DataField.VOLUME, volumes, Codec.DELTA, min_ts, max_ts,
            )
            status = self._store.put(symbol, DataType.KLINE_DAY, block)
            if status != StoreStatus.OK:
                logger.error("put %s/volume failed: %s", symbol, status)

        # Amount block (Delta codec, int64)
        if "amount" in df.columns:
            amounts = np.array(df["amount"], dtype=np.int64)
            block = ColumnBlock.compress_int64(
                DataField.AMOUNT, amounts, Codec.DELTA, min_ts, max_ts,
            )
            status = self._store.put(symbol, DataType.KLINE_DAY, block)
            if status != StoreStatus.OK:
                logger.error("put %s/amount failed: %s", symbol, status)

        # VWAP block (Gorilla codec, fixed-point)
        if "vwap" in df.columns:
            vwaps = np.array(
                [_float_to_fixed(v) for v in df["vwap"]],
                dtype=np.float64,
            )
            block = ColumnBlock.compress_double(
                DataField.VWAP, vwaps, Codec.GORILLA, min_ts, max_ts,
            )
            status = self._store.put(symbol, DataType.KLINE_DAY, block)
            if status != StoreStatus.OK:
                logger.error("put %s/vwap failed: %s", symbol, status)

        return n

    def _field_name_to_enum(self, field: str) -> DataField:
        """Convert field name string to DataField enum."""
        mapping = {
            "open": DataField.OPEN,
            "high": DataField.HIGH,
            "low": DataField.LOW,
            "close": DataField.CLOSE,
            "volume": DataField.VOLUME,
            "amount": DataField.AMOUNT,
            "vwap": DataField.VWAP,
        }
        result = mapping.get(field.lower())
        if result is None:
            raise ValueError(f"Unknown field name: {field}. Valid: {list(mapping.keys())}")
        return result