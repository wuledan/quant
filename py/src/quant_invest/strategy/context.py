#!/usr/bin/env python3
"""策略上下文

为策略提供统一的运行时环境：
- 数据访问
- 因子计算（调用C++引擎）
- 日志记录
- 参数管理
"""

from __future__ import annotations

from datetime import date
from typing import TYPE_CHECKING

import pandas as pd

from ..data.providers.base import DataProvider

if TYPE_CHECKING:
    from .factor_api import FactorAPI


class StrategyContext:
    """策略上下文"""

    def __init__(
        self,
        data_handler: object | None = None,
        factor_api: FactorAPI | None = None,
        data_provider: DataProvider | None = None,
    ) -> None:
        self._data_handler = data_handler
        self._factor_api = factor_api
        self._data_provider = data_provider
        self._logs: list[dict] = []

    def current_bar(self, symbol: str) -> dict | None:
        """获取当前K线数据"""
        if self._data_handler is None:
            raise RuntimeError("DataHandler not available")
        return self._data_handler.current_bar(symbol)

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取最近N根K线"""
        if self._data_handler is None:
            raise RuntimeError("DataHandler not available")
        return self._data_handler.history(symbol, bars)

    def get_factor(
        self,
        name: str,
        symbols: list[str],
        date: date,
    ) -> pd.Series:
        """调用C++因子引擎计算因子"""
        if self._factor_api is None:
            raise RuntimeError("Factor API not available")
        return self._factor_api.calculate(name, symbols, date)

    def log(self, message: str, level: str = "INFO") -> None:
        """记录策略日志"""
        self._logs.append({"message": message, "level": level})

    def get_trading_calendar(self, start: date, end: date) -> list[date]:
        """获取交易日历"""
        if self._data_provider is None:
            raise RuntimeError("DataProvider not available for trading calendar")
        return self._data_provider.get_trade_calendar(start, end)

    def get_logs(self) -> list[dict]:
        """获取策略日志。"""
        return self._logs.copy()
