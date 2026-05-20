#!/usr/bin/env python3
"""P4 集成测试 — 验证 Kline→Factor→Signal→Risk→Order 端到端管道.

测试场景:
1. KlinePipeline 全链路管道
2. RiskEngineAdapter 风控检查
3. EventBusBridge 事件推送
4. 端到端数据一致性
"""

import sys
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "py" / "src"))


class TestP4Integration:
    """P4 集成测试."""

    def test_kline_pipeline_basic(self) -> None:
        """测试 KlinePipeline 基本流程."""
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

        # 模拟 K 线
        prices = [10.0, 10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 12.8, 12.5, 12.0,
                  11.5, 11.0, 10.5, 10.0, 9.5, 9.0, 8.5, 8.0, 7.5, 7.0,
                  8.0, 9.0, 10.0, 11.0, 12.0]

        for price in prices:
            result = pipeline.on_bar("TEST", {"close": price})

        # 因子值应已计算
        assert result.factor_result is not None
        assert result.factor_result.factor_values is not None
        assert "fast_ma" in result.factor_result.factor_values

        stats = pipeline.get_stats()
        assert stats["total_bars"] == 25
        assert stats["factor_computes"] == 25

        bus.stop()

    def test_kline_pipeline_events(self) -> None:
        """测试 KlinePipeline 事件推送."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above
        from quant_invest.api.event_bus import EventBusBridge

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        bus = EventBusBridge()
        bus.start()

        # 监听所有事件
        kline_events = []
        factor_events = []
        bus.subscribe("kline", lambda t, d: kline_events.append(d))
        bus.subscribe("factor", lambda t, d: factor_events.append(d))

        pipeline = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline.initialize()

        prices = [10.0, 10.5, 11.0, 11.5, 12.0]
        for price in prices:
            pipeline.on_bar("TEST", {"close": price})

        assert len(kline_events) == 5, "应有 5 个 kline 事件"
        assert len(factor_events) == 5, "应有 5 个 factor 事件"

        bus.stop()

    def test_risk_adapter_order_check(self) -> None:
        """测试 RiskEngineAdapter 订单风控."""
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        adapter = RiskEngineAdapter()

        # 正常订单
        result = adapter.check_order("000001.SH", "BUY", 10.0, 1000)
        assert result.passed, "正常订单应通过"
        assert result.risk_level == "LOW"

        # 超额订单
        result2 = adapter.check_order("600519.SH", "BUY", 1800.0, 1000)
        assert not result2.passed, "超额订单应被拦截"
        assert len(result2.violations) > 0

    def test_risk_adapter_portfolio_check(self) -> None:
        """测试 RiskEngineAdapter 组合风控."""
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        adapter = RiskEngineAdapter()

        # 正常组合
        adapter.update_position("000001.SH", 200_000, 1_000_000)
        result = adapter.check_portfolio()
        assert result.passed, "正常组合应通过"

        # 高集中度
        adapter.update_position("000001.SH", 400_000, 1_000_000)
        result2 = adapter.check_portfolio()
        assert not result2.passed, "高集中度应被拦截"

    def test_risk_adapter_drawdown_check(self) -> None:
        """测试 RiskEngineAdapter 回撤检查."""
        from quant_invest.risk.risk_adapter import RiskEngineAdapter

        adapter = RiskEngineAdapter()

        # 模拟回撤
        adapter.update_position("000001.SH", 100_000, 1_000_000)  # peak
        adapter.update_position("000001.SH", 100_000, 850_000)  # -15%

        result = adapter.check_portfolio()
        assert not result.passed, "回撤超限应被拦截"
        assert any("回撤" in v for v in result.violations)

    def test_pipeline_with_risk(self) -> None:
        """测试 KlinePipeline + RiskEngine 集成."""
        from quant_invest.pipeline.kline_pipeline import KlinePipeline
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above
        from quant_invest.api.event_bus import EventBusBridge

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        bus = EventBusBridge()
        bus.start()

        risk_events = []
        bus.subscribe("risk", lambda t, d: risk_events.append(d))

        pipeline = KlinePipeline(MAStrategy(), event_bus=bus)
        pipeline.initialize()

        prices = [10.0, 10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 12.8, 12.5, 12.0,
                  11.5, 11.0, 10.5, 10.0, 9.5, 9.0, 8.5, 8.0, 7.5, 7.0,
                  8.0, 9.0, 10.0, 11.0, 12.0]

        for price in prices:
            result = pipeline.on_bar("TEST", {"close": price})

        # 管道应正常完成
        stats = pipeline.get_stats()
        assert stats["total_bars"] == 25

        bus.stop()

    def test_pipeline_latency(self) -> None:
        """测试 KlinePipeline 延迟性能."""
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

        # 模拟 100 根 K 线
        import math
        prices = [10.0 + math.sin(i * 0.3) * 2 for i in range(100)]

        for price in prices:
            pipeline.on_bar("TEST", {"close": price})

        stats = pipeline.get_stats()
        avg_latency_us = stats["avg_latency_us"]
        assert avg_latency_us < 1000, f"平均延迟 {avg_latency_us:.0f}μs 应低于 1ms"

        bus.stop()
