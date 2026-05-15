#!/usr/bin/env python3
"""事件驱动回测引擎

核心循环：
1. DataHandler 产生 MarketEvent
2. MarketEvent 分发给所有 Strategy
3. Strategy 产生 SignalEvent
4. SignalEvent 分发给 Portfolio 生成 OrderEvent
5. OrderEvent 分发给 SimulatedBroker 执行
6. FillEvent 返回 Portfolio 更新持仓
7. 重复直至数据耗尽
"""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from typing import Any

import pandas as pd

from ..strategy.base import StrategyBase
from .broker import SimulatedBroker
from .data_handler import DataHandler
from .performance import PerformanceAnalyzer, PerformanceMetrics
from .portfolio import Portfolio


@dataclass
class BacktestResult:
    """回测结果."""

    start_date: date
    end_date: date
    initial_cash: float
    final_value: float
    total_return: float
    total_trades: int
    metrics: PerformanceMetrics | None = None
    nav_series: pd.Series | None = None
    trades: list[dict[str, Any]] = field(default_factory=list)


class BacktestEngine:
    """事件驱动回测引擎"""

    def __init__(
        self,
        data_handler: DataHandler,
        broker: SimulatedBroker,
        portfolio: Portfolio,
        strategy: StrategyBase,
        initial_cash: float = 1_000_000.0,
        benchmark: str = "000300.SH",
    ) -> None:
        self.data_handler = data_handler
        self.broker = broker
        self.portfolio = portfolio
        self.strategy = strategy
        self.initial_cash = initial_cash
        self.benchmark = benchmark
        self._continue_backtest: bool = True

    def run(
        self,
        start_date: date,
        end_date: date,
        frequency: str = "daily",
    ) -> BacktestResult:
        """执行回测主循环."""
        self._initialize(start_date)
        while self._continue_backtest:
            self._tick()
        return self._generate_result(start_date, end_date)

    def _tick(self) -> None:
        """单步事件循环."""
        # 1. 获取下一个市场数据
        market_event = self.data_handler.next_bar()
        if market_event is None:
            self._continue_backtest = False
            return

        # 2. 策略处理市场数据，生成信号
        signals = self.strategy.on_bar(
            market_event.bar_data or {},
            self.portfolio.current_positions,
        )

        # 3. 组合管理：信号 → 目标仓位 → 订单
        orders = self.portfolio.generate_orders(signals, market_event)

        # 4. 模拟执行
        fills = self.broker.execute_orders(orders, market_event)

        # 5. 更新持仓与净值
        self.portfolio.update_from_fills(fills, market_event.timestamp)
        self.portfolio.record_snapshot(market_event.timestamp)

    def _initialize(self, start_date: date) -> None:
        """初始化回测状态."""
        self.portfolio.initialize(self.initial_cash)
        self.strategy.on_init()

    def _generate_result(self, start_date: date, end_date: date) -> BacktestResult:
        """生成回测结果报告."""
        nav_df = self.portfolio.get_nav_dataframe()

        if nav_df.empty:
            return BacktestResult(
                start_date=start_date,
                end_date=end_date,
                initial_cash=self.initial_cash,
                final_value=self.initial_cash,
                total_return=0.0,
                total_trades=0,
            )

        # 构建净值序列
        nav_series = nav_df["total_value"] if "total_value" in nav_df.columns else None

        # 计算绩效指标
        if nav_series is not None and len(nav_series) > 1:
            analyzer = PerformanceAnalyzer()
            try:
                metrics = analyzer.analyze(nav_df, pd.Series(dtype=float))
            except Exception:
                metrics = None
        else:
            metrics = None

        final_value = float(nav_series.iloc[-1]) if nav_series is not None else self.initial_cash
        total_return = (final_value / self.initial_cash) - 1.0

        return BacktestResult(
            start_date=start_date,
            end_date=end_date,
            initial_cash=self.initial_cash,
            final_value=final_value,
            total_return=total_return,
            total_trades=len(self.portfolio._nav_history),
            metrics=metrics,
            nav_series=nav_series,
        )
