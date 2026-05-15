"""数据源提供者基类

所有数据源（akshare/tushare/wind）均需实现此接口。
统一的接口使得上层代码不依赖具体数据源。
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from datetime import date, datetime
from enum import Enum

import pandas as pd


class DataFreq(str, Enum):
    """数据频率"""

    TICK = "tick"
    MIN_1 = "1min"
    MIN_5 = "5min"
    MIN_15 = "15min"
    MIN_30 = "30min"
    MIN_60 = "60min"
    DAILY = "daily"
    WEEKLY = "weekly"
    MONTHLY = "monthly"


class AdjustMethod(str, Enum):
    """复权方式"""

    NONE = "none"
    FORWARD = "forward"
    BACKWARD = "backward"


class DataProvider(ABC):
    """数据源提供者抽象基类"""

    @property
    @abstractmethod
    def name(self) -> str:
        """数据源名称，如 'akshare', 'tushare', 'wind'"""
        ...

    @abstractmethod
    def get_daily_bars(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        """获取日行情数据

        Returns:
            DataFrame with columns: [open, high, low, close, volume, amount, turnover]
            Index: DatetimeIndex (trade_date)
        """
        ...

    @abstractmethod
    def get_minute_bars(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        """获取分钟线数据"""
        ...

    @abstractmethod
    def get_tick_data(
        self,
        symbol: str,
        trade_date: date,
    ) -> pd.DataFrame:
        """获取Tick级别数据"""
        ...

    @abstractmethod
    def get_financial_data(
        self,
        symbol: str,
        report_type: str = "income",
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> pd.DataFrame:
        """获取财务数据（利润表/资产负债表/现金流量表）"""
        ...

    @abstractmethod
    def get_index_constituents(
        self,
        index_symbol: str,
        trade_date: date | None = None,
    ) -> pd.DataFrame:
        """获取指数成分股"""
        ...

    @abstractmethod
    def get_industry_classification(
        self,
        symbol: str | None = None,
        level: str = "L1",
    ) -> pd.DataFrame:
        """获取行业分类（申万/中信行业分类）"""
        ...

    @abstractmethod
    def get_trade_calendar(
        self,
        start_date: date,
        end_date: date,
    ) -> list[date]:
        """获取交易日历"""
        ...

    @abstractmethod
    def health_check(self) -> bool:
        """数据源健康检查"""
        ...
