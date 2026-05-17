#!/usr/bin/env python3
"""分钟线数据采集器

支持多种频率(1min/5min/15min/30min/60min)，
流式处理按日切分，增量更新。
"""

from __future__ import annotations

import json
import time
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Any

import pandas as pd

from ..providers.base import AdjustMethod, DataFreq, DataProvider
from ..quality.validator import DataValidator, QualityLevel


class MinuteBarCollector:
    """分钟线数据采集器"""

    def __init__(
        self,
        provider: DataProvider,
        storage_path: str = "./data/minute",
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
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        """采集分钟线数据。

        按日流式处理：将日区间拆分为单个交易日，
        逐日获取并追加写入Parquet。
        """
        trade_dates = pd.bdate_range(start_date, end_date).tolist()
        if not trade_dates:
            return self._load_existing(symbol, freq)

        progress_key = self._progress_key(symbol, freq)
        last_date = self._get_progress(progress_key)

        all_fetched: list[pd.DataFrame] = []
        for td in trade_dates:
            day = td.date() if hasattr(td, 'date') else td
            if last_date is not None and day <= last_date:
                continue

            start_time = datetime(day.year, day.month, day.day, 9, 30)
            end_time = datetime(day.year, day.month, day.day, 15, 0)

            df = self._fetch_with_retry(symbol, start_time, end_time, freq, adjust)
            if df.empty:
                continue

            self._validate(symbol, df, day, day)
            self._append_parquet(symbol, freq, df)
            all_fetched.append(df)
            self._save_progress(progress_key, day)

        return self._load_existing(symbol, freq)

    def get_progress(self, symbol: str, freq: DataFreq = DataFreq.MIN_1) -> date | None:
        """获取某标的最新数据日期。"""
        return self._get_progress(self._progress_key(symbol, freq))

    def clear_progress(self, symbol: str | None = None, freq: DataFreq | None = None) -> None:
        """清除进度记录。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return
        progress: dict = json.loads(path.read_text())
        if symbol and freq:
            progress.pop(self._progress_key(symbol, freq), None)
        elif symbol:
            prefix = f"{symbol}_"
            for k in list(progress):
                if k.startswith(prefix):
                    progress.pop(k, None)
        else:
            progress.clear()
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))

    # ── 内部方法 ──────────────────────────────────────────────

    def _fetch_with_retry(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq,
        adjust: AdjustMethod,
    ) -> pd.DataFrame:
        """带指数退避重试的数据获取。"""
        last_exc: Exception | None = None
        wait = self._retry_delay
        for attempt in range(self._max_retries):
            try:
                return self._provider.get_minute_bars(
                    symbol=symbol,
                    start_time=start_time,
                    end_time=end_time,
                    freq=freq,
                    adjust=adjust,
                )
            except Exception as e:
                last_exc = e
                if attempt < self._max_retries - 1:
                    time.sleep(wait)
                    wait *= self._retry_backoff
        raise RuntimeError(
            f"MinuteBarCollector.collect({symbol}) failed after "
            f"{self._max_retries} retries"
        ) from last_exc

    def _validate(
        self,
        symbol: str,
        df: pd.DataFrame,
        start_date: date,
        end_date: date,
    ) -> None:
        """数据质量校验。"""
        report = self._validator.validate(df, symbol, start_date, end_date)
        if report.level == QualityLevel.ERROR:
            raise ValueError(f"数据质量校验失败: {report.summary}")

    def _append_parquet(self, symbol: str, freq: DataFreq, df: pd.DataFrame) -> None:
        """追加写入Parquet文件（按symbol+频率分片）。"""
        path = self._parquet_path(symbol, freq)
        if path.exists():
            existing = pd.read_parquet(path)
            combined = pd.concat([existing, df])
            combined = combined[~combined.index.duplicated(keep="last")]
            combined.sort_index(inplace=True)
            combined.to_parquet(path, index=True)
        else:
            df.to_parquet(path, index=True)

    def _load_existing(self, symbol: str, freq: DataFreq) -> pd.DataFrame:
        """加载本地已有数据。"""
        path = self._parquet_path(symbol, freq)
        if not path.exists():
            return pd.DataFrame()
        return pd.read_parquet(path)

    def _parquet_path(self, symbol: str, freq: DataFreq) -> Path:
        """Parquet文件路径。"""
        safe = symbol.replace(".", "_").replace(" ", "_")
        return self._storage_path / f"{safe}_{freq.value}.parquet"

    def _progress_key(self, symbol: str, freq: DataFreq) -> str:
        """进度键名。"""
        return f"{symbol}_{freq.value}"

    def _get_progress(self, key: str) -> date | None:
        """读取进度。"""
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

    def _save_progress(self, key: str, last_date: date) -> None:
        """持久化进度。"""
        path = self._storage_path / "progress.json"
        progress: dict = {}
        if path.exists():
            progress = json.loads(path.read_text())
        progress[key] = {
            "last_date": last_date.isoformat(),
            "updated_at": datetime.now().isoformat(),
        }
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))
