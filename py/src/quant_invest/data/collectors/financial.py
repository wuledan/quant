#!/usr/bin/env python3
"""财务数据采集器

三大报表(利润表/资产负债表/现金流量表)分别获取，
增量更新(按报告期)。
"""

from __future__ import annotations

import json
import time
from datetime import date, datetime
from pathlib import Path
from typing import Any

import pandas as pd

from ..providers.base import DataProvider
from ..quality.validator import DataValidator, QualityLevel


class FinancialDataCollector:
    """财务数据采集器"""

    REPORT_TYPES = ["income", "balance", "cashflow"]

    def __init__(
        self,
        provider: DataProvider,
        storage_path: str = "./data/financial",
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
        report_type: str = "income",
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> pd.DataFrame:
        """采集财务数据。

        增量更新：读取已采集的最新报告期，仅获取新报告期数据。
        三大报表独立存储。
        """
        if report_type not in self.REPORT_TYPES:
            raise ValueError(f"Unknown report_type: {report_type}, choose from {self.REPORT_TYPES}")

        progress_key = self._progress_key(symbol, report_type)
        last_date = self._get_progress(progress_key)

        effective_start = start_date
        if last_date is not None:
            from datetime import timedelta
            candidate = last_date + timedelta(days=1)
            effective_start = max(candidate, effective_start) if effective_start else candidate

        if effective_start and end_date and effective_start >= end_date:
            return self._load_existing(symbol, report_type)

        df = self._fetch_with_retry(symbol, report_type, effective_start, end_date)
        if df.empty:
            return self._load_existing(symbol, report_type)

        self._validate(symbol, df, start_date or date.min, end_date or date.max)
        self._save_parquet(symbol, report_type, df)
        self._save_progress(progress_key, end_date or date.today())

        return self._load_existing(symbol, report_type)

    def collect_all(
        self,
        symbol: str,
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> dict[str, pd.DataFrame]:
        """采集全部三大报表。"""
        results: dict[str, pd.DataFrame] = {}
        for rt in self.REPORT_TYPES:
            results[rt] = self.collect(
                symbol=symbol, report_type=rt, start_date=start_date, end_date=end_date
            )
        return results

    def get_progress(self, symbol: str, report_type: str = "income") -> date | None:
        """获取某标的最新数据日期。"""
        return self._get_progress(self._progress_key(symbol, report_type))

    def clear_progress(self, symbol: str | None = None, report_type: str | None = None) -> None:
        """清除进度记录。"""
        path = self._storage_path / "progress.json"
        if not path.exists():
            return
        progress: dict = json.loads(path.read_text())
        if symbol and report_type:
            progress.pop(self._progress_key(symbol, report_type), None)
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
        report_type: str,
        start_date: date | None,
        end_date: date | None,
    ) -> pd.DataFrame:
        """带重试的数据获取。"""
        last_exc: Exception | None = None
        wait = self._retry_delay
        for attempt in range(self._max_retries):
            try:
                return self._provider.get_financial_data(
                    symbol=symbol,
                    report_type=report_type,
                    start_date=start_date,
                    end_date=end_date,
                )
            except Exception as e:
                last_exc = e
                if attempt < self._max_retries - 1:
                    time.sleep(wait)
                    wait *= self._retry_backoff
        raise RuntimeError(
            f"FinancialDataCollector.collect({symbol}, {report_type}) failed after "
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

    def _save_parquet(self, symbol: str, report_type: str, new_df: pd.DataFrame) -> None:
        """追加写入Parquet。"""
        path = self._parquet_path(symbol, report_type)
        if path.exists():
            existing = pd.read_parquet(path)
            combined = pd.concat([existing, new_df])
            combined = combined[~combined.index.duplicated(keep="last")]
            combined.sort_index(inplace=True)
            combined.to_parquet(path, index=True)
        else:
            new_df.to_parquet(path, index=True)

    def _load_existing(self, symbol: str, report_type: str) -> pd.DataFrame:
        """加载本地数据。"""
        path = self._parquet_path(symbol, report_type)
        if not path.exists():
            return pd.DataFrame()
        return pd.read_parquet(path)

    def _parquet_path(self, symbol: str, report_type: str) -> Path:
        """Parquet文件路径。"""
        safe = symbol.replace(".", "_").replace(" ", "_")
        return self._storage_path / f"{safe}_{report_type}.parquet"

    def _progress_key(self, symbol: str, report_type: str) -> str:
        return f"{symbol}_{report_type}"

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

    def _save_progress(self, key: str, last_date: date) -> None:
        path = self._storage_path / "progress.json"
        progress: dict = {}
        if path.exists():
            progress = json.loads(path.read_text())
        progress[key] = {
            "last_date": last_date.isoformat(),
            "updated_at": datetime.now().isoformat(),
        }
        path.write_text(json.dumps(progress, indent=2, sort_keys=True))
