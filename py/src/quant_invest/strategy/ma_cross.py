#!/usr/bin/env python3
"""均线交叉策略 - 具体策略实现."""

from __future__ import annotations

from datetime import date

import pandas as pd

from quant_invest.backtest import BacktestEngine, BacktestResult
from quant_invest.backtest.broker import SimulatedBroker
from quant_invest.backtest.data_handler import DailyDataHandler
from quant_invest.backtest.portfolio import Portfolio
from quant_invest.data.providers import DataProvider, DataProviderFactory
from quant_invest.strategy.base import StrategyBase
from quant_invest.strategy.registry import StrategyRegistry
register = StrategyRegistry.register


@register("ma_cross")
class MACrossStrategy(StrategyBase):
    """双均线交叉策略.

    逻辑:
    - fast_ma 上穿 slow_ma → 买入
    - fast_ma 下穿 slow_ma → 卖出
    """

    def __init__(self, fast_period: int = 5, slow_period: int = 20) -> None:
        super().__init__()
        self.fast_period = fast_period
        self.slow_period = slow_period
        self._prev_fast: float | None = None
        self._prev_slow: float | None = None
        self._position = 0

    def on_init(self, context):
        context.log(f"均线交叉策略初始化: fast={self.fast_period}, slow={self.slow_period}")
        self._prev_fast = None
        self._prev_slow = None
        self._position = 0

    def on_bar(self, bar_data: dict[str, pd.DataFrame], positions: dict) -> dict[str, float]:
        signals: dict[str, float] = {}

        for symbol, df in bar_data.items():
            if df is None or len(df) < self.slow_period:
                continue

            close = df["close"]
            fast_ma = close.rolling(self.fast_period).mean().iloc[-1]
            slow_ma = close.rolling(self.slow_period).mean().iloc[-1]

            if self._prev_fast is not None and self._prev_slow is not None:
                # 金叉: fast 上穿 slow
                if self._prev_fast <= self._prev_slow and fast_ma > slow_ma:
                    signals[symbol] = 1.0
                    self._position = 1
                # 死叉: fast 下穿 slow
                elif self._prev_fast >= self._prev_slow and fast_ma < slow_ma:
                    signals[symbol] = -1.0
                    self._position = 0

            self._prev_fast = fast_ma
            self._prev_slow = slow_ma

        return signals

    def on_finish(self, context):
        context.log(f"均线交叉策略运行结束, 最终持仓: {self._position}")


def run_ma_cross_backtest(
    symbols: list[str],
    start_date: date,
    end_date: date,
    initial_cash: float = 1_000_000,
    fast_period: int = 5,
    slow_period: int = 20,
    provider: DataProvider | None = None,
) -> BacktestResult:
    """运行均线交叉策略回测."""
    provider = provider or DataProviderFactory.create("yahoo")
    strategy = MACrossStrategy(fast_period=fast_period, slow_period=slow_period)
    data_handler = DailyDataHandler(symbols, start_date, end_date, provider)
    portfolio = Portfolio()
    portfolio.initialize(initial_cash)
    broker = SimulatedBroker()

    engine = BacktestEngine(
        initial_cash=initial_cash,
        strategy=strategy,
        data_handler=data_handler,
        portfolio=portfolio,
        broker=broker,
    )

    return engine.run(start_date=start_date, end_date=end_date)
