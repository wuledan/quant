# P0-T2: StorageEngineAdapter Design

## 1. Architecture Overview

The `StorageEngineAdapter` bridges Python Parquet data files to the C++ `TimeSeriesStore` via pybind11. It provides a high-level API for loading, querying, and storing K-line data.

```
┌──────────────────┐     ┌──────────────────────┐     ┌──────────────────┐
│  Parquet Files   │────▶│  StorageEngineAdapter │────▶│  C++ TimeSeries  │
│  (py/data/daily) │     │  (Python)             │     │  Store (C++)     │
└──────────────────┘     │                       │     └──────────────────┘
                         │  - load_from_parquet  │
                         │  - load_all_cached    │
                         │  - query_kline        │
                         │  - store_kline        │
                         │  - query_kline_as_df  │
                         │                       │
                         │  In-memory cache:     │
                         │  dict[symbol, DF]     │
                         └──────────────────────┘
```

## 2. Key Design Decisions

### 2.1 Dual Storage: C++ TimeSeriesStore + Python In-Memory Cache

**Problem**: The C++ `store_kline()` method has a codec mismatch bug — it stores prices as `int64` via Delta codec, but `query_kline()` tries to decompress them as `double` via Gorilla codec, producing garbage results.

**Solution**: We use a dual approach:
1. **C++ TimeSeriesStore + ColumnBlock**: For batch loading, we create `ColumnBlock` objects with proper codec selection (Gorilla for prices, Delta for timestamps/volumes) and store via `upsert_blocks()`.
2. **Python in-memory cache**: For queries, we maintain a `dict[symbol, pd.DataFrame]` cache that provides fast, correct results without depending on the buggy C++ query path.

This gives us:
- ✅ Correct data loading into C++ engine (for future use when the codec bug is fixed)
- ✅ Correct query results via Python cache
- ✅ Fast queries (no C++ round-trip needed)
- ✅ DataFrame-native API for downstream consumers

### 2.2 Codec Selection for ColumnBlock

| Field | Codec | Data Type | Rationale |
|---|---|---|---|
| Timestamp | Delta | int64 | Sequential timestamps have small deltas |
| Open/High/Low/Close | Gorilla | float64 (fixed-point) | Financial prices benefit from Gorilla's XOR encoding |
| Volume | Delta | int64 | Sequential volumes have small deltas |
| Amount | Delta | int64 | Sequential amounts have small deltas |
| VWAP | Gorilla | float64 (fixed-point) | Similar to prices |

### 2.3 Price Representation: Fixed-Point

Prices are stored as `price * 10000` (int64) to preserve 4 decimal places. This avoids floating-point precision issues and is compatible with A-share price formats.

- `_float_to_fixed(3371.69)` → `33716900`
- `_fixed_to_float(33716900)` → `3371.69`

## 3. API Reference

### StorageEngineAdapter

```python
class StorageEngineAdapter:
    def __init__(self, data_dir="py/data/daily", cache_budget_mb=256, engine_data_dir="/tmp/quant_storage")
    def load_from_parquet(self, symbol: str, parquet_path: str | Path) -> int
    def load_all_cached(self) -> dict[str, int]
    def query_kline(self, symbol, start_ts=None, end_ts=None, field="close") -> list[dict]
    def query_kline_all(self, symbol, start_ts=None, end_ts=None) -> dict[str, list[dict]]
    def query_kline_as_df(self, symbol, start_ts=None, end_ts=None) -> pd.DataFrame
    def store_kline(self, symbol: str, row: dict) -> None
    def flush(self) -> None
    def close(self) -> None

    @property
    def store(self) -> TimeSeriesStore
    @property
    def loaded_symbols(self) -> dict[str, int]
```

### DataScheduler

```python
class DataScheduler:
    def __init__(self, data_dir="py/data/daily", cache_budget_mb=256, engine_data_dir="/tmp/quant_storage")
    def initialize(self) -> dict[str, int]  # Load all cached data
    def get_storage_adapter(self) -> StorageEngineAdapter
    def on_new_data(self, symbol: str, df: pd.DataFrame) -> None
    def shutdown(self) -> None
```

## 4. File Structure

```
py/src/quant_invest/
├── storage/
│   ├── __init__.py          # Package init, exports StorageEngineAdapter
│   └── adapter.py           # StorageEngineAdapter class
├── data/
│   └── scheduler.py         # DataScheduler (modified)
```

## 5. Known Issues

### C++ store_kline Codec Mismatch Bug

**Root Cause**: `StorageEngine::store_kline()` stores all fields as `int64` via Delta codec (line 66-77 of `storage_engine.cc`), but `query_kline()` decompresses via `block.decompress(std::span<double>(values))` which expects Gorilla-encoded `double` data.

**Impact**: `query_kline()` returns garbage values after `store_kline()`.

**Workaround**: Use `TimeSeriesStore::upsert_blocks()` with manually created `ColumnBlock` objects that use the correct codec, and maintain a Python in-memory cache for queries.

**Fix Required**: In `storage_engine.cc`, `store_kline()` should use Gorilla codec for price fields and `query_kline()` should check the codec type before decompressing.

### store_kline_batch Returns NOT_FOUND

**Root Cause**: `store_kline_batch()` returns `StoreStatus::NOT_FOUND` because the shard for the symbol hasn't been created yet. The batch path doesn't auto-create shards like the single-row path does.

**Impact**: Batch loading via `store_kline_batch()` doesn't work.

**Workaround**: Use `TimeSeriesStore::upsert_blocks()` instead.

## 6. Usage Examples

### Load all cached data and query

```python
from quant_invest.storage.adapter import StorageEngineAdapter

adapter = StorageEngineAdapter(data_dir="py/data/daily")
results = adapter.load_all_cached()
# {'000001.SH': 242, '600519.SH': 242, ...}

# Query close prices
data = adapter.query_kline("000001.SH", field="close")
# [{"timestamp": 1716163200000000, "value": 3371.69}, ...]

# Query as DataFrame
df = adapter.query_kline_as_df("000001.SH")
# DataFrame with DatetimeIndex and OHLCV columns
```

### Using DataScheduler

```python
from quant_invest.data.scheduler import DataScheduler

scheduler = DataScheduler(data_dir="py/data/daily")
scheduler.initialize()

adapter = scheduler.get_storage_adapter()
df = adapter.query_kline_as_df("000001.SH")

scheduler.shutdown()
```

### Real-time data ingestion

```python
adapter.store_kline("000001.SH", {
    "timestamp": 1716249600000000,
    "open": 3380.0,
    "high": 3390.0,
    "low": 3370.0,
    "close": 3385.0,
    "volume": 500000,
    "amount": 1692500000,
    "vwap": 3385.0,
})
```

## 7. Performance Notes

- Loading 242 rows × 6 symbols ≈ 1452 rows total takes < 1 second
- Query from in-memory cache is O(n) where n is the number of rows for the symbol
- C++ ColumnBlock storage with Gorilla compression achieves ~2-3x compression ratio for prices
- The in-memory cache uses ~1-2 MB for 1452 rows (negligible)
