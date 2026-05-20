#!/usr/bin/env python3
"""K线→因子→信号全链路管道 — 端到端事件驱动管道.

将 K 线数据通过因子计算、信号生成、订单执行的全流程串联:

    KlineEvent → FactorEngineBridge → SignalEvent → OrderAdapter → FillEvent

每个环节通过 EventBusBridge 发布事件，支持:
1. 同步管道（回测模式）
2. 异步管道（实时模式，通过 EventBus 推送）

用法::

    from quant_invest.pipeline.kline_pipeline import KlinePipeline

    pipeline = KlinePipeline(strategy, event_bus)
    pipeline.initialize()

    # 每根 K 线触发
    result = pipeline.on_bar("000001.SH", {"close": 3400.5, "volume": 1000000})
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Any

from ..api.event_bus import EventBusBridge, get_event_bus
from ..execution.order_adapter import OrderAdapter, OrderResult
from ..strategy.factor_engine import FactorEngineBridge, FactorComputeResult
from ..strategy.dsl import Strategy, SignalContext

logger = logging.getLogger("quant_invest.pipeline.kline_pipeline")


@dataclass
class PipelineResult:
    """管道处理结果."""

    symbol: str
    factor_result: FactorComputeResult | None = None
    signal_fired: bool = False
    signal_direction: str = ""  # "BUY" / "SELL" / ""
    order_result: OrderResult | None = None
    latency_ns: int = 0


class KlinePipeline:
    """K线→因子→信号全链路管道.

    串联:
    1. FactorEngineBridge: K 线 → 因子计算 → 信号
    2. Strategy.on_signal(): 信号 → 订单决策
    3. OrderAdapter: 订单 → 模拟成交

    每个环节通过 EventBusBridge 发布事件:
    - "kline": K 线数据到达
    - "factor": 因子计算完成
    - "signal": 信号触发
    - "order": 订单提交
    - "fill": 成交回报
    """

    def __init__(
        self,
        strategy: Strategy,
        event_bus: EventBusBridge | None = None,
        use_cpp_execution: bool = False,
    ) -> None:
        self._strategy = strategy
        self._event_bus = event_bus or get_event_bus()
        self._use_cpp_execution = use_cpp_execution

        # 因子引擎
        self._factor_bridge = FactorEngineBridge(strategy)

        # 订单适配器
        self._order_adapter: OrderAdapter | None = None
        if use_cpp_execution:
            self._init_order_adapter()

        # 管道统计
        self._stats = _PipelineStats()

    def _init_order_adapter(self) -> None:
        """初始化订单适配器."""
        try:
            adapter = OrderAdapter()
            if adapter.cpp_available:
                adapter.connect_broker()
                self._order_adapter = adapter
                logger.info("C++ OrderAdapter 已启用")
            else:
                logger.info("C++ OrderAdapter 不可用，使用 Python 回退")
                self._order_adapter = OrderAdapter()
                self._order_adapter._cpp_available = False
                self._order_adapter.connect_broker()
        except Exception as e:
            logger.warning("OrderAdapter 初始化失败: %s", e)
            self._order_adapter = None

    def initialize(self) -> None:
        """初始化管道."""
        self._factor_bridge.initialize()
        logger.info(
            "KlinePipeline 初始化: %d 因子, %d 信号, C++执行=%s",
            len(self._factor_bridge._factors),
            len(self._factor_bridge._signals),
            self._order_adapter is not None and self._order_adapter.cpp_available,
        )

    def on_bar(
        self,
        symbol: str,
        bar_data: dict[str, float],
        cash: float = 1_000_000.0,
        position: float = 0.0,
    ) -> PipelineResult:
        """处理一根 K 线数据，执行全链路管道.

        Args:
            symbol: 标的代码
            bar_data: K 线数据 {"close": 3400.5, "volume": 1000000, ...}
            cash: 当前现金
            position: 当前持仓

        Returns:
            PipelineResult
        """
        start_ns = time.perf_counter_ns()
        self._stats.total_bars += 1

        # 1. 发布 K 线事件
        self._event_bus.publish("kline", {
            "symbol": symbol,
            **bar_data,
        })

        # 2. 因子计算
        factor_result = self._factor_bridge.on_bar(symbol, bar_data)
        self._stats.factor_computes += 1

        # 发布因子事件
        if factor_result.factor_values:
            self._event_bus.publish("factor", {
                "symbol": symbol,
                "values": factor_result.factor_values,
                "signals": factor_result.signal_values,
                "cpp_used": factor_result.cpp_used,
                "compute_time_ns": factor_result.compute_time_ns,
            })

        # 3. 信号处理
        signal_fired = False
        signal_direction = ""
        order_result = None

        if factor_result.signal_values and hasattr(self._strategy, 'on_signal'):
            # 检查是否有非零信号
            for name, value in factor_result.signal_values.items():
                if value != 0.0:
                    signal_fired = True
                    signal_direction = "BUY" if value > 0 else "SELL"

                    # 发布信号事件
                    self._event_bus.publish("signal", {
                        "symbol": symbol,
                        "name": name,
                        "value": value,
                        "direction": signal_direction,
                    })

                    # 调用策略 on_signal
                    price = bar_data.get("close", 0.0)
                    ctx = SignalContext(
                        symbol=symbol,
                        price=price,
                        cash=cash,
                        position=position,
                    )
                    self._strategy.on_signal(ctx)

                    # 处理策略产生的订单
                    if ctx.orders:
                        for order in ctx.orders:
                            order_result = self._execute_order(order, symbol, price)
                            self._stats.total_orders += 1

                    break  # 只处理第一个非零信号

        elapsed = time.perf_counter_ns() - start_ns
        self._stats.total_latency_ns += elapsed

        return PipelineResult(
            symbol=symbol,
            factor_result=factor_result,
            signal_fired=signal_fired,
            signal_direction=signal_direction,
            order_result=order_result,
            latency_ns=elapsed,
        )

    def _execute_order(self, order: dict, symbol: str, price: float) -> OrderResult:
        """执行订单.

        Args:
            order: 策略产生的订单 {"side": "BUY", "qty": 100, ...}
            symbol: 标的代码
            price: 当前价格

        Returns:
            OrderResult
        """
        side = order.get("side", "BUY")
        qty = order.get("qty", 100)
        order_type = order.get("type", "MARKET")

        # 发布订单事件
        self._event_bus.publish("order", {
            "symbol": symbol,
            "side": side,
            "quantity": qty,
            "price": price,
            "type": order_type,
        })

        if self._order_adapter is not None:
            result = self._order_adapter.submit_order(
                symbol=symbol,
                side=side,
                order_type=order_type,
                price=price,
                quantity=qty,
            )

            # 发布成交事件
            if result.success and result.filled_qty > 0:
                self._event_bus.publish("fill", {
                    "symbol": symbol,
                    "side": side,
                    "filled_qty": result.filled_qty,
                    "fill_price": result.fill_price,
                    "order_id": result.order_id,
                })

            return result

        # 无 OrderAdapter 时返回模拟结果
        return OrderResult(
            order_id=0,
            status="FILLED",
            filled_qty=qty,
            fill_price=price,
        )

    def get_stats(self) -> dict[str, Any]:
        """获取管道统计."""
        avg_latency = (
            self._stats.total_latency_ns / self._stats.total_bars
            if self._stats.total_bars > 0
            else 0
        )
        return {
            "total_bars": self._stats.total_bars,
            "factor_computes": self._stats.factor_computes,
            "total_orders": self._stats.total_orders,
            "avg_latency_ns": avg_latency,
            "avg_latency_us": avg_latency / 1000,
        }

    def reset(self) -> None:
        """重置管道状态."""
        self._factor_bridge.reset()
        self._stats = _PipelineStats()


class _PipelineStats:
    """管道统计."""

    def __init__(self) -> None:
        self.total_bars: int = 0
        self.factor_computes: int = 0
        self.total_orders: int = 0
        self.total_latency_ns: int = 0
