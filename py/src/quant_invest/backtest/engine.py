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

import logging
from dataclasses import dataclass, field
from datetime import date
from typing import Any, TYPE_CHECKING

import pandas as pd

from .broker import SimulatedBroker

if TYPE_CHECKING:
    from ..strategy.base import StrategyBase
from .events import FillEvent
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


class _BacktestContext:
    """Simple strategy context passed to on_init/on_finish."""
    def log(self, msg: str) -> None:
        import logging
        logging.getLogger("quant_invest.backtest").info("[Strategy] %s", msg)


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
        on_progress: Any | None = None,
    ) -> None:
        self.data_handler = data_handler
        self.broker = broker
        self.portfolio = portfolio
        self.strategy = strategy
        self.initial_cash = initial_cash
        self.benchmark = benchmark
        self._ctx = _BacktestContext()
        self._continue_backtest: bool = True
        self._fills: list[FillEvent] = []
        self._logger = logging.getLogger("quant_invest.backtest")
        # 进度回调: on_progress(current_bar, total_bars, message)
        self._on_progress = on_progress

    def run(
        self,
        start_date: date,
        end_date: date,
        frequency: str = "daily",
    ) -> BacktestResult:
        """执行回测主循环."""
        self._logger.info("回测开始: %s ~ %s, 初始资金=%.0f", start_date, end_date, self.initial_cash)
        self._initialize(start_date)
        total_bars = len(self.data_handler._dates)
        bar_count = 0
        while self._continue_backtest:
            self._tick()
            bar_count += 1
            if self._on_progress and bar_count % 20 == 0:
                self._on_progress(bar_count, total_bars, f"处理第 {bar_count}/{total_bars} 根K线")
        self._logger.info("回测结束: 共处理 %d 根K线, %d 笔成交", bar_count, len(self._fills))
        if self._on_progress:
            self._on_progress(total_bars, total_bars, "回测完成，生成结果")
        return self._generate_result(start_date, end_date)

    def _tick(self) -> None:
        """单步事件循环."""
        # 1. 获取下一个市场数据
        market_event = self.data_handler.next_bar()
        if market_event is None:
            self._continue_backtest = False
            return

        # 2. 构建多K线DataFrame供策略使用
        bar_dataframes: dict[str, pd.DataFrame] = {}
        for symbol in market_event.bar_data or {}:
            hist = self.data_handler.history(symbol, bars=30)
            if not hist.empty:
                bar_dataframes[symbol] = hist

        raw_signals = self.strategy.on_bar(
            bar_dataframes,
            self.portfolio.current_positions,
        )

        # 3. 将策略输出的dict信号转为SignalEvent列表
        from .events import SignalEvent
        signals: list[SignalEvent] = []
        if isinstance(raw_signals, dict):
            for symbol, value in raw_signals.items():
                if value is None:
                    continue
                direction = "LONG" if value > 0 else ("SHORT" if value < 0 else "EXIT")
                signals.append(SignalEvent(
                    timestamp=market_event.timestamp,
                    symbol=symbol,
                    direction=direction,
                    strength=abs(value),
                ))

        if signals:
            self._logger.debug("产生信号: %s", [(s.symbol, s.direction) for s in signals])

        # 4. 组合管理：信号 → 目标仓位 → 订单
        orders = self.portfolio.generate_orders(signals, market_event)

        # 5. 模拟执行
        fills = self.broker.execute_orders(orders, market_event)
        self._fills.extend(fills)

        if fills:
            self._logger.info("成交: %s", [(f.symbol, f.direction, f.quantity, f.fill_price) for f in fills])

        # 6. 更新持仓与净值 — 用当前市价更新持仓市值
        self.portfolio.update_from_fills(fills, market_event.timestamp)
        self._update_market_value(market_event)
        self.portfolio.record_snapshot(market_event.timestamp)

    def _initialize(self, start_date: date) -> None:
        """初始化回测状态."""
        self.portfolio.initialize(self.initial_cash)
        self.strategy.on_init(self._ctx)
        self._logger.info("策略初始化完成, 数据bar数: %d", len(self.data_handler._dates))

    def _update_market_value(self, market_event: MarketEvent) -> None:
        """用当前市价更新持仓市值."""
        bar_data = market_event.bar_data or {}
        for symbol, pos in self.portfolio.positions.items():
            symbol_data = bar_data.get(symbol, {})
            if isinstance(symbol_data, dict) and "close" in symbol_data:
                pos.market_value = pos.quantity * symbol_data["close"]
            elif isinstance(symbol_data, pd.Series) and "close" in symbol_data:
                pos.market_value = pos.quantity * float(symbol_data["close"])

    def _generate_result(self, start_date: date, end_date: date) -> BacktestResult:
        """生成回测结果报告."""
        nav_df = self.portfolio.get_nav_dataframe()

        if nav_df.empty:
            self._logger.warning("净值数据为空，无有效回测结果")
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
        metrics = None
        if nav_series is not None and len(nav_series) > 1:
            analyzer = PerformanceAnalyzer()
            try:
                metrics = analyzer.analyze(nav_df, pd.Series(dtype=float))
            except Exception as e:
                self._logger.warning("绩效指标计算失败: %s", e)

        final_value = float(nav_series.iloc[-1]) if nav_series is not None else self.initial_cash
        total_return = (final_value / self.initial_cash) - 1.0

        # 构建交易记录
        trade_records = []
        for fill in self._fills:
            trade_records.append({
                "date": fill.timestamp.strftime("%Y-%m-%d") if hasattr(fill.timestamp, "strftime") else str(fill.timestamp),
                "symbol": fill.symbol,
                "direction": "BUY" if fill.direction == "BUY" else "SELL",
                "quantity": fill.quantity,
                "price": round(fill.fill_price, 2),
            })

        self._logger.info(
            "回测结果: 总收益率=%.4f, 最终资产=%.2f, 交易次数=%d",
            total_return, final_value, len(trade_records),
        )

        return BacktestResult(
            start_date=start_date,
            end_date=end_date,
            initial_cash=self.initial_cash,
            final_value=final_value,
            total_return=total_return,
            total_trades=len(trade_records),
            metrics=metrics,
            nav_series=nav_series,
            trades=trade_records,
        )
