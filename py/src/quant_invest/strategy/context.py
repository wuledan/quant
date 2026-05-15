"""策略上下文

为策略提供统一的运行时环境：
- 数据访问
- 因子计算（调用C++引擎）
- 日志记录
- 参数管理
"""

from __future__ import annotations

from datetime import date

import pandas as pd


class StrategyContext:
    """策略上下文"""

    def __init__(
        self,
        data_handler: object | None = None,
        factor_api: FactorAPI | None = None,
    ) -> None:
        self._data_handler = data_handler
        self._factor_api = factor_api
        self._logs: list[dict] = []

    def current_bar(self, symbol: str) -> dict | None:
        """获取当前K线数据"""
        # TODO: 实现
        raise NotImplementedError("StrategyContext.current_bar not implemented")

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取最近N根K线"""
        if self._data_handler is None:
            raise RuntimeError("DataHandler not available")
        return self._data_handler.history(symbol, bars)  # type: ignore[attr-defined]

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
        # TODO: 实现
        raise NotImplementedError("StrategyContext.get_trading_calendar not implemented")
