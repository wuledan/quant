#!/usr/bin/env python3
"""行业分类数据采集器

支持L1/L2行业分级查询。
"""

from __future__ import annotations

import json
import time
from datetime import date, datetime
from pathlib import Path

import pandas as pd

from ..providers.base import DataProvider


class IndustryClassificationCollector:
    """行业分类数据采集器"""

    def __init__(
        self,
        provider: DataProvider,
        storage_path: str = "./data/industry",
        max_retries: int = 3,
        retry_delay: float = 1.0,
        retry_backoff: float = 2.0,
    ) -> None:
        self._provider = provider
        self._storage_path = Path(storage_path)
        self._storage_path.mkdir(parents=True, exist_ok=True)
        self._max_retries = max_retries
        self._retry_delay = retry_delay
        self._retry_backoff = retry_backoff

    @property
    def provider(self) -> DataProvider:
        return self._provider

    # ── 公开接口 ──────────────────────────────────────────────

    def collect(
        self,
        level: str = "L1",
        symbol: str | None = None,
    ) -> pd.DataFrame:
        """采集行业分类数据。"""
        df = self._fetch_with_retry(symbol, level)
        if df.empty:
            return self._load_existing(level, symbol)

        self._save_parquet(level, symbol, df)
        progress_key = self._progress_key(level, symbol)
        self._save_progress(progress_key, datetime.now().isoformat())
        return self._load_existing(level, symbol)

    def get_classification(
        self,
        symbol: str,
        level: str = "L1",
    ) -> dict:
        """获取某只股票的行业分类。"""
        df = self._load_existing(level, symbol)
        if df.empty:
            df = self._load_existing(level, None)
        if df.empty:
            df = self._fetch_with_retry(symbol, level)
        if df.empty:
            return {}
        result: dict = {}
        for col in df.columns:
            if len(df) > 0:
                val = df.iloc[0][col]
                result[col] = val if not pd.isna(val) else None
        return result

    def get_progress(self, level: str = "L1", symbol: str | None = None) -> str | None:
        """获取采集进度。"""
        key = self._progress_key(level, symbol)
        return self._get_progress(key)

    def clear_progress(self, level: str | None = None, symbol: str | None = None) -> None:
        """清除进度。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return
        progress: dict = {}
        try:
            progress = json.loads(path.read_text())
        except (json.JSONDecodeError, FileNotFoundError):
            pass
        if level and symbol:
            progress.pop(self._progress_key(level, symbol), None)
        elif level:
            prefix = f"{level}_"
            for k in list(progress):
                if k.startswith(prefix):
                    progress.pop(k, None)
        else:
            progress.clear()
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))

    # ── 内部方法 ──────────────────────────────────────────────

    def _fetch_with_retry(self, symbol: str | None, level: str) -> pd.DataFrame:
        """带重试的数据获取。"""
        last_exc: Exception | None = None
        wait = self._retry_delay
        for attempt in range(self._max_retries):
            try:
                return self._provider.get_industry_classification(
                    symbol=symbol,
                    level=level,
                )
            except Exception as e:
                last_exc = e
                if attempt < self._max_retries - 1:
                    time.sleep(wait)
                    wait *= self._retry_backoff
        raise RuntimeError(
            f"IndustryClassificationCollector.collect(level={level}) "
            f"failed after {self._max_retries} retries"
        ) from last_exc

    def _save_parquet(self, level: str, symbol: str | None, df: pd.DataFrame) -> None:
        """写入Parquet。"""
        path = self._parquet_path(level, symbol)
        df.to_parquet(path, index=True)

    def _load_existing(self, level: str, symbol: str | None) -> pd.DataFrame:
        """加载本地数据。"""
        path = self._parquet_path(level, symbol)
        if not path.exists():
            return pd.DataFrame()
        return pd.read_parquet(path)

    def _parquet_path(self, level: str, symbol: str | None) -> Path:
        safe_sym = (symbol or "all").replace(".", "_").replace(" ", "_")
        return self._storage_path / f"{level}_{safe_sym}.parquet"

    def _progress_key(self, level: str, symbol: str | None) -> str:
        return f"{level}_{symbol or '__all__'}"

    def _get_progress(self, key: str) -> str | None:
        path = self._storage_path / "progress.json"
        if not path.exists():
            return None
        try:
            progress: dict = json.loads(path.read_text())
        except (json.JSONDecodeError, FileNotFoundError):
            return None
        raw = progress.get(key)
        if isinstance(raw, str):
            return raw
        if isinstance(raw, dict):
            return raw.get("last_date")
        return None

    def _save_progress(self, key: str, value: str) -> None:
        path = self._storage_path / "progress.json"
        progress: dict = {}
        if path.exists():
            try:
                progress = json.loads(path.read_text())
            except (json.JSONDecodeError, FileNotFoundError):
                pass
        progress[key] = {
            "last_date": value,
            "updated_at": datetime.now().isoformat(),
        }
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))
