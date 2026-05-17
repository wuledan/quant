#!/usr/bin/env python3
"""指数成分股数据采集器

支持成分股变动检测(diff)。
"""

from __future__ import annotations

import json
import time
from datetime import date, datetime
from pathlib import Path
from typing import Any

import pandas as pd

from ..providers.base import DataProvider


class IndexConstituentCollector:
    """指数成分股数据采集器"""

    def __init__(
        self,
        provider: DataProvider,
        storage_path: str = "./data/index_constituent",
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
        index_symbol: str,
        trade_date: date | None = None,
    ) -> pd.DataFrame:
        """采集指数成分股。"""
        df = self._fetch_with_retry(index_symbol, trade_date)
        if df.empty:
            return self._load_existing(index_symbol)

        self._save_parquet(index_symbol, df, trade_date)
        self._save_progress(index_symbol, trade_date or date.today())
        return self._load_existing(index_symbol)

    def diff(
        self,
        index_symbol: str,
        date_a: date,
        date_b: date,
    ) -> dict[str, list[str]]:
        """检测两个日期的成分股变动。

        Returns:
            dict with keys:
            - added: date_b 新增的股票
            - removed: date_a 有但 date_b 没有的股票
        """
        df_a = self._fetch_with_retry(index_symbol, date_a)
        df_b = self._fetch_with_retry(index_symbol, date_b)

        constituents_a = self._extract_constituents(df_a)
        constituents_b = self._extract_constituents(df_b)

        set_a = set(constituents_a)
        set_b = set(constituents_b)

        return {
            "added": sorted(set_b - set_a),
            "removed": sorted(set_a - set_b),
        }

    def get_progress(self, index_symbol: str) -> date | None:
        """获取指数的最新采集日期。"""
        return self._get_progress(index_symbol)

    def clear_progress(self, index_symbol: str | None = None) -> None:
        """清除进度。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return
        progress: dict = json.loads(path.read_text())
        if index_symbol:
            progress.pop(index_symbol, None)
        else:
            progress.clear()
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))

    # ── 内部方法 ──────────────────────────────────────────────

    def _fetch_with_retry(
        self,
        index_symbol: str,
        trade_date: date | None,
    ) -> pd.DataFrame:
        """带重试的数据获取。"""
        last_exc: Exception | None = None
        wait = self._retry_delay
        for attempt in range(self._max_retries):
            try:
                return self._provider.get_index_constituents(
                    index_symbol=index_symbol,
                    trade_date=trade_date,
                )
            except Exception as e:
                last_exc = e
                if attempt < self._max_retries - 1:
                    time.sleep(wait)
                    wait *= self._retry_backoff
        raise RuntimeError(
            f"IndexConstituentCollector.collect({index_symbol}) failed after "
            f"{self._max_retries} retries"
        ) from last_exc

    def _save_parquet(self, index_symbol: str, df: pd.DataFrame, trade_date: date | None) -> None:
        """写入Parquet并按日期快照存储。"""
        path = self._parquet_path(index_symbol)
        if "date" not in df.columns and trade_date:
            df = df.copy()
            df["snapshot_date"] = trade_date.isoformat()
        if path.exists():
            existing = pd.read_parquet(path)
            combined = pd.concat([existing, df])
            combined = combined[~combined.index.duplicated(keep="last")]
            combined.to_parquet(path, index=True)
        else:
            df.to_parquet(path, index=True)

    def _load_existing(self, index_symbol: str) -> pd.DataFrame:
        """加载本地数据。"""
        path = self._parquet_path(index_symbol)
        if not path.exists():
            return pd.DataFrame()
        return pd.read_parquet(path)

    def _parquet_path(self, index_symbol: str) -> Path:
        safe = index_symbol.replace(".", "_").replace(" ", "_")
        return self._storage_path / f"{safe}.parquet"

    @staticmethod
    def _extract_constituents(df: pd.DataFrame) -> list[str]:
        """提取成分股代码列表。"""
        if df.empty:
            return []
        for col in ["constituent_code", "code", "stock_code", "symbol", "con_code"]:
            if col in df.columns:
                return df[col].astype(str).tolist()
        if len(df.columns) > 0:
            return df.iloc[:, 0].astype(str).tolist()
        return []

    def _get_progress(self, key: str) -> date | None:
        path = self._storage_path / "progress.json"
        if not path.exists():
            return None
        progress: dict = json.loads(path.read_text())
        raw = progress.get(key)
        if raw is None:
            return None
        if isinstance(raw, str):
            return date.fromisoformat(raw)
        if isinstance(raw, dict):
            return date.fromisoformat(raw.get("last_date", ""))
        return None

    def _save_progress(self, key: str, dt: date) -> None:
        path = self._storage_path / "progress.json"
        progress: dict = {}
        if path.exists():
            progress = json.loads(path.read_text())
        progress[key] = {
            "last_date": dt.isoformat(),
            "updated_at": datetime.now().isoformat(),
        }
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))
