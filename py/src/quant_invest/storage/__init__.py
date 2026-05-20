"""Storage adapter — Python wrapper around C++ StorageEngine via pybind11.

Provides high-level API for:
- Loading Parquet data into the C++ StorageEngine
- Querying K-line data from the C++ engine
- Real-time single-row ingestion
"""

from quant_invest.storage.adapter import StorageEngineAdapter

__all__ = ["StorageEngineAdapter"]
