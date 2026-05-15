"""增量更新调度器."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from enum import Enum

import pandas as pd

from quant_invest.data.providers.base import DataProvider


class UpdateMode(str, Enum):
    """更新模式."""

    FULL = "full"
    INCREMENTAL = "incremental"
    REPAIR = "repair"


@dataclass
class UpdateTask:
    """增量更新任务."""

    provider: str
    data_type: str
    symbols: list[str]
    mode: UpdateMode
    start_date: date | None = None
    end_date: date | None = None
    priority: int = 0


class DataScheduler:
    """增量更新调度器."""

    def __init__(self, config: dict | None = None) -> None:
        self._providers: dict[str, DataProvider] = {}
        self._progress: dict[str, date] = {}

    def register_provider(self, name: str, provider: DataProvider) -> None:
        """注册数据源."""
        self._providers[name] = provider

    async def run_task(self, task: UpdateTask) -> pd.DataFrame:
        """执行更新任务."""
        raise NotImplementedError("DataScheduler.run_task 尚未实现")

    async def run_daily_update(self) -> dict[str, bool]:
        """执行每日增量更新."""
        raise NotImplementedError("DataScheduler.run_daily_update 尚未实现")

    def get_progress(self, symbol: str) -> date | None:
        """获取某标的最新数据日期."""
        return self._progress.get(symbol)
