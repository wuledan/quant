"""本地缓存管理."""

from __future__ import annotations

from pathlib import Path

import pandas as pd


class CacheManager:
    """本地Parquet缓存管理器."""

    def __init__(self, cache_path: str = "./cache") -> None:
        self._cache_path = Path(cache_path)
        self._cache_path.mkdir(parents=True, exist_ok=True)

    def get(self, key: str) -> pd.DataFrame | None:
        """从缓存读取."""
        path = self._cache_path / f"{key}.parquet"
        if path.exists():
            return pd.read_parquet(path)
        return None

    def put(self, key: str, df: pd.DataFrame) -> None:
        """写入缓存."""
        path = self._cache_path / f"{key}.parquet"
        df.to_parquet(path, index=True)

    def exists(self, key: str) -> bool:
        """检查缓存是否存在."""
        return (self._cache_path / f"{key}.parquet").exists()

    def clear(self, key: str) -> None:
        """清除指定缓存."""
        path = self._cache_path / f"{key}.parquet"
        if path.exists():
            path.unlink()
