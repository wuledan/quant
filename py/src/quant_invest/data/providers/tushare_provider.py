"""Tushare数据源适配器"""

from __future__ import annotations

from datetime import date, datetime

import pandas as pd

from .base import AdjustMethod, DataFreq, DataProvider


class TushareProvider(DataProvider):
    """Tushare Pro数据源适配器"""

    def __init__(self, token: str, config: dict | None = None) -> None:
        self._token = token
        self._config = config or {}

    @property
    def name(self) -> str:
        return "tushare"

    def get_daily_bars(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_daily_bars not implemented")

    def get_minute_bars(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_minute_bars not implemented")

    def get_tick_data(self, symbol: str, trade_date: date) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_tick_data not implemented")

    def get_financial_data(
        self,
        symbol: str,
        report_type: str = "income",
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_financial_data not implemented")

    def get_index_constituents(
        self,
        index_symbol: str,
        trade_date: date | None = None,
    ) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_index_constituents not implemented")

    def get_industry_classification(
        self,
        symbol: str | None = None,
        level: str = "L1",
    ) -> pd.DataFrame:
        raise NotImplementedError("TushareProvider.get_industry_classification not implemented")

    def get_trade_calendar(self, start_date: date, end_date: date) -> list[date]:
        raise NotImplementedError("TushareProvider.get_trade_calendar not implemented")

    def health_check(self) -> bool:
        return True
