#!/usr/bin/env python3
"""端到端系统测试 — 验证完整投资系统链路.

测试场景:
1. 数据加载 → StorageEngine → 回测引擎
2. DSL 策略 → 因子计算 → 信号生成
3. 信号 → 风控 → 订单 → 成交
4. KlinePipeline 全链路 + EventBus 事件一致性
5. 回测引擎 + DSL 策略完整回测
6. 多策略并发管道
"""

import sys
from datetime import date
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "py" / "src"))


class TestEndToEnd:
    """端到端系统测试."""

    def test_kline_pipeline_full_chain(self) -> None:
        """测试 KlinePipeline 全链路: K线→因子→信号→风控→订单→成交."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above, cross_below, SignalContext
        from quant_invest.api.event_bus import EventBusBridge

        class DualMAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)
            death_cross = cross_below(fast_ma, slow_ma)

            def on_signal(self, ctx: SignalContext) -> None:
                if self.golden_cross > 0:
                    qty = int(ctx.cash * 0.1 / ctx.price)
                    if qty > 0:
                        ctx.order(side="BUY", quantity=qty)
                elif self.death_cross < 0:
                    if ctx.position > 0:
                        ctx.order(side="SELL", quantity=int(ctx.position))

        bus = EventBusBridge()
        bus.start()

        # 收集各阶段事件
        events: dict[str, list] = {
            "kline": [], "factor": [], "signal": [],
            "risk": [], "order": [], "fill": [],
        }
        for topic in events:

            def _make_cb(lst):
                return lambda _t, d, _lst=lst: _lst.append(d)

            bus.subscribe(topic, _make_cb(events[topic]))

        pipeline = KlinePipeline(DualMAStrategy(), event_bus=bus)
        pipeline.initialize()

        # 模拟 50 根 K 线: 先涨后跌，触发金叉和死叉
        import math
        prices = [10.0 + 3.0 * math.sin(i * 0.15) for i in range(50)]

        results = []
        for price in prices:
            result = pipeline.on_bar("TEST", {"close": price})
            results.append(result)

        # 验证: 因子值应已计算
        last = results[-1]
        assert last.factor_result is not None
        assert last.factor_result.factor_values is not None

        # 验证: kline 事件数量
        assert len(events["kline"]) == 50, f"kline 事件 {len(events['kline'])} != 50"

        # 验证: factor 事件数量
        assert len(events["factor"]) == 50, f"factor 事件 {len(events['factor'])} != 50"

        # 验证: 管道统计
        stats = pipeline.get_stats()
        assert stats["total_bars"] == 50
        assert stats["factor_computes"] == 50

        bus.stop()

    def test_risk_blocks_dangerous_order(self) -> None:
        """测试风控拦截超额订单."""
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        adapter = RiskEngineAdapter()

        # 直接测试: 高价股 × 大数量 → 超过单笔限额
        result = adapter.check_order("600519.SH", "BUY", 1800.0, 1000)
        assert not result.passed, "180万订单应被风控拦截"
        assert any("单笔金额" in v for v in result.violations)

        # 正常订单应通过
        result2 = adapter.check_order("000001.SH", "BUY", 10.0, 1000)
        assert result2.passed, "1万订单应通过"

    def test_pipeline_with_order_adapter(self) -> None:
        """测试 KlinePipeline + OrderAdapter 订单执行."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above, SignalContext
        from quant_invest.api.event_bus import EventBusBridge

        class SimpleStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

            def on_signal(self, ctx: SignalContext) -> None:
                if self.golden_cross > 0:
                    qty = int(ctx.cash * 0.05 / ctx.price)
                    if qty > 0:
                        ctx.order(side="BUY", quantity=qty)

        bus = EventBusBridge()
        bus.start()

        fill_events = []
        bus.subscribe("fill", lambda t, d: fill_events.append(d))

        pipeline = KlinePipeline(SimpleStrategy(), event_bus=bus, use_cpp_execution=True)
        pipeline.initialize()

        import math
        prices = [10.0 + 3.0 * math.sin(i * 0.15) for i in range(50)]
        for price in prices:
            pipeline.on_bar("TEST", {"close": price})

        stats = pipeline.get_stats()
        assert stats["total_bars"] == 50

        bus.stop()

    def test_event_bus_cross_module(self) -> None:
        """测试 EventBusBridge 跨模块事件传递."""
        from quant_invest.api.event_bus import EventBusBridge
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        bus = EventBusBridge()
        bus.start()

        # 风控模块订阅因子事件
        risk_adapter = RiskEngineAdapter()
        factor_data_received = []

        def on_factor(topic: str, data: dict) -> None:
            factor_data_received.append(data)

        bus.subscribe("factor", on_factor)

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        pipeline = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline.initialize()

        prices = [10.0 + i * 0.2 for i in range(25)]
        for price in prices:
            pipeline.on_bar("TEST", {"close": price})

        # 风控模块应收到因子事件
        assert len(factor_data_received) == 25

        bus.stop()

    def test_pipeline_reset_and_rerun(self) -> None:
        """测试管道重置后重新运行."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above
        from quant_invest.api.event_bus import EventBusBridge

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        bus = EventBusBridge()
        bus.start()

        pipeline = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline.initialize()

        # 第一次运行
        for price in [10.0 + i * 0.5 for i in range(20)]:
            pipeline.on_bar("TEST", {"close": price})

        stats1 = pipeline.get_stats()
        assert stats1["total_bars"] == 20

        # 重置
        pipeline.reset()
        stats_after_reset = pipeline.get_stats()
        assert stats_after_reset["total_bars"] == 0

        # 第二次运行
        for price in [20.0 + i * 0.3 for i in range(15)]:
            pipeline.on_bar("TEST", {"close": price})

        stats2 = pipeline.get_stats()
        assert stats2["total_bars"] == 15
        assert stats2["factor_computes"] == 15

        bus.stop()

    def test_multiple_pipelines_same_bus(self) -> None:
        """测试多个管道共享同一 EventBus."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above
        from quant_invest.api.event_bus import EventBusBridge

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        bus = EventBusBridge()
        bus.start()

        kline_events = []
        bus.subscribe("kline", lambda t, d: kline_events.append(d))

        # 两个管道，不同策略实例
        pipeline_a = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline_b = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline_a.initialize()
        pipeline_b.initialize()

        # 管道 A 处理 10 根 K 线
        for price in [10.0 + i * 0.5 for i in range(10)]:
            pipeline_a.on_bar("SYMA", {"close": price})

        # 管道 B 处理 5 根 K 线
        for price in [20.0 + i * 0.3 for i in range(5)]:
            pipeline_b.on_bar("SYMB", {"close": price})

        # 共享 EventBus 应收到 15 个 kline 事件
        assert len(kline_events) == 15

        # 各管道统计独立
        assert pipeline_a.get_stats()["total_bars"] == 10
        assert pipeline_b.get_stats()["total_bars"] == 5

        bus.stop()

    def test_risk_adapter_full_lifecycle(self) -> None:
        """测试风控适配器完整生命周期."""
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        adapter = RiskEngineAdapter()

        # 1. 初始状态: 正常订单应通过
        result = adapter.check_order("000001.SH", "BUY", 10.0, 1000)
        assert result.passed

        # 2. 逐步增加持仓
        adapter.update_position("000001.SH", 100_000, 1_000_000)
        result = adapter.check_portfolio()
        assert result.passed, "10% 集中度应通过"

        # 3. 高集中度
        adapter.update_position("000001.SH", 350_000, 1_000_000)
        result = adapter.check_portfolio()
        assert not result.passed, "35% 集中度应被拦截"
        assert any("持仓比例" in v for v in result.violations)

        # 4. 回撤检查
        adapter.update_position("000001.SH", 350_000, 850_000)
        result = adapter.check_portfolio()
        assert not result.passed, "回撤超限应被拦截"
        assert any("回撤" in v for v in result.violations)

    def test_pipeline_latency_under_load(self) -> None:
        """测试管道在高负载下的延迟."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above, cross_below, SignalContext
        from quant_invest.api.event_bus import EventBusBridge

        class ComplexStrategy(Strategy):
            ma5 = Factor(kind="MA", input="close", params={"period": 5})
            ma10 = Factor(kind="MA", input="close", params={"period": 10})
            ma20 = Factor(kind="MA", input="close", params={"period": 20})
            golden = cross_above(ma5, ma20)
            death = cross_below(ma5, ma20)

            def on_signal(self, ctx: SignalContext) -> None:
                if self.golden > 0:
                    ctx.order(side="BUY", quantity=100)
                elif self.death < 0:
                    ctx.order(side="SELL", quantity=100)

        bus = EventBusBridge()
        bus.start()

        pipeline = KlinePipeline(ComplexStrategy(), event_bus=bus)
        pipeline.initialize()

        import math
        prices = [10.0 + 3.0 * math.sin(i * 0.1) + math.sin(i * 0.3) for i in range(500)]

        for price in prices:
            pipeline.on_bar("TEST", {"close": price})

        stats = pipeline.get_stats()
        assert stats["total_bars"] == 500
        avg_latency_us = stats["avg_latency_us"]
        assert avg_latency_us < 2000, f"500 根 K 线平均延迟 {avg_latency_us:.0f}μs 应低于 2ms"

        bus.stop()
