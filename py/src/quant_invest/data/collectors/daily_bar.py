#!/usr/bin/env python3
"""日行情数据采集器

支持增量更新：读取本地进度 → 仅获取增量数据 → 批量写入Parquet。
"""

from __future__ import annotations

import json
import time
from datetime import date, datetime
from pathlib import Path
from typing import Any

import pandas as pd

from ..providers.base import AdjustMethod, DataProvider
from ..quality.validator import DataValidator, QualityLevel


class DailyBarCollector:
    """日行情数据采集器

    负责从数据源获取日线行情数据，执行数据质量校验后存储。
    """

    def __init__(
        self,
        provider: DataProvider,
        storage_path: str = "./data/daily",
        validator: DataValidator | None = None,
        max_retries: int = 3,
        retry_delay: float = 1.0,
        retry_backoff: float = 2.0,
    ) -> None:
        self._provider = provider
        self._storage_path = Path(storage_path)
        self._storage_path.mkdir(parents=True, exist_ok=True)
        self._validator = validator or DataValidator()
        self._max_retries = max_retries
        self._retry_delay = retry_delay
        self._retry_backoff = retry_backoff

    @property
    def provider(self) -> DataProvider:
        return self._provider

    # ── 公开接口 ──────────────────────────────────────────────

    def collect(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        """采集日线数据。

        流程：
        1. 读取本地进度(progress.json)
        2. 增量计算有效起止日期
        3. 从DataProvider获取数据（带重试）
        4. DataValidator数据质量校验
        5. 批量写入Parquet
        6. 更新进度文件
        """
        effective_start = self._calc_effective_start(symbol, start_date)
        if effective_start >= end_date:
            return self._load_existing(symbol, end_date)

        df = self._fetch_with_retry(symbol, effective_start, end_date, adjust)
        if df.empty:
            return self._load_existing(symbol, end_date)

        self._validate(symbol, df, effective_start, end_date)
        self._save_parquet(symbol, df)
        self._save_progress(symbol, end_date)

        return self._load_existing(symbol, end_date)

    def get_progress(self, symbol: str) -> date | None:
        """获取某标的最新数据日期。"""
        progress = self._load_progress()
        raw = progress.get(symbol)
        if raw is None:
            return None
        if isinstance(raw, str):
            return date.fromisoformat(raw)
        if isinstance(raw, dict):
            return date.fromisoformat(raw.get("last_date", ""))
        return None

    def clear_progress(self, symbol: str | None = None) -> None:
        """清除进度记录（用于强制全量刷新）。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return
        progress = self._load_progress()
        if symbol:
            progress.pop(symbol, None)
        else:
            progress.clear()
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))

    # ── 内部方法 ──────────────────────────────────────────────

    def _calc_effective_start(self, symbol: str, start_date: date) -> date:
        """基于本地进度计算有效起始日期。"""
        last = self.get_progress(symbol)
        if last is None:
            return start_date
        from datetime import timedelta

        candidate = last + timedelta(days=1)
        return max(candidate, start_date)

    def _fetch_with_retry(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod,
    ) -> pd.DataFrame:
        """带指数退避重试的数据获取。"""
        last_exc: Exception | None = None
        wait = self._retry_delay
        for attempt in range(self._max_retries):
            try:
                return self._provider.get_daily_bars(
                    symbol=symbol,
                    start_date=start_date,
                    end_date=end_date,
                    adjust=adjust,
                )
            except Exception as e:
                last_exc = e
                if attempt < self._max_retries - 1:
                    time.sleep(wait)
                    wait *= self._retry_backoff
        raise RuntimeError(
            f"DailyBarCollector.collect({symbol}) failed after "
            f"{self._max_retries} retries"
        ) from last_exc

    def _validate(
        self,
        symbol: str,
        df: pd.DataFrame,
        start_date: date,
        end_date: date,
    ) -> None:
        """数据质量校验，ERROR级别抛异常。"""
        report = self._validator.validate(df, symbol, start_date, end_date)
        if report.level == QualityLevel.ERROR:
            raise ValueError(f"数据质量校验失败: {report.summary}")

    def _normalize_index(self, df: pd.DataFrame) -> pd.DataFrame:
        """确保索引为无时区的DatetimeIndex，避免Parquet读写时区问题."""
        if isinstance(df.index, pd.DatetimeIndex):
            if df.index.tz is not None:
                df.index = df.index.tz_localize(None)
        return df

    def _save_parquet(self, symbol: str, new_df: pd.DataFrame) -> None:
        """追加写入Parquet文件。"""
        new_df = self._normalize_index(new_df)
        path = self._parquet_path(symbol)
        if path.exists():
            existing = pd.read_parquet(path)
            combined = pd.concat([existing, new_df])
            combined = combined[~combined.index.duplicated(keep="last")]
            combined.sort_index(inplace=True)
            combined.to_parquet(path, index=True)
        else:
            new_df.to_parquet(path, index=True)

    def _load_existing(self, symbol: str, end_date: date) -> pd.DataFrame:
        """加载本地已有数据（用于无增量时返回缓存）。"""
        path = self._parquet_path(symbol)
        if not path.exists():
            return pd.DataFrame()
        try:
            df = pd.read_parquet(path)
        except Exception:
            return pd.DataFrame()
        return df[df.index <= pd.Timestamp(end_date)]

    def _parquet_path(self, symbol: str) -> Path:
        """Parquet文件路径。"""
        safe = symbol.replace(".", "_")
        return self._storage_path / f"{safe}.parquet"

    def _load_progress(self) -> dict[str, Any]:
        """加载本地进度记录。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return {}
        return json.loads(path.read_text())

    def _save_progress(self, symbol: str, last_date: date) -> None:
        """持久化进度。"""
        path = self._storage_path / "progress.json"
        progress = self._load_progress()
        progress[symbol] = {
            "last_date": last_date.isoformat(),
            "updated_at": datetime.now().isoformat(),
        }
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))
