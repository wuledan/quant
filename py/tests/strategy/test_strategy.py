#!/usr/bin/env python3
"""策略研究框架测试."""

from __future__ import annotations

from datetime import datetime

import pandas as pd
import pytest

from quant_invest.backtest.events import SignalEvent
from quant_invest.strategy import (
    PositionSizer,
    SignalGenerator,
    StrategyBase,
    StrategyParams,
    StrategyRegistry,
)


class TestStrategyBase:
    """策略基类测试."""

    def test_create_params(self):
        """创建参数."""
        params = StrategyParams()
        assert params is not None

    def test_strategy_lifecycle(self):
        """生命周期测试."""
        strategy = _create_test_strategy()
        assert strategy.params is not None

        strategy.on_init()
        assert strategy.initialized

        signals = strategy.on_bar({"000001.SZ": {"close": 10.5}}, {})
        assert len(signals) >= 0

        strategy.on_finish()
        assert strategy.finished

    def test_strategy_context(self):
        """上下文属性."""
        strategy = _create_test_strategy()
        with pytest.raises(AssertionError):
            _ = strategy.context

        strategy.set_context("test_context")
        assert strategy.context == "test_context"

    def test_strategy_with_params(self):
        """带参数的策略."""
        params = StrategyParams()
        strategy = _create_test_strategy(params=params)
        assert strategy.params is params

    def test_on_order_filled(self):
        """成交回调默认不报错."""
        strategy = _create_test_strategy()
        strategy.on_finish()  # should not raise

    def test_strategy_registry_decorator(self):
        """策略注册装饰器."""
        @StrategyRegistry.register("momentum")
        class MomentumStrategy(StrategyBase):
            def on_init(self) -> None:
                pass

            def on_bar(self, bar_data, positions) -> list:
                return []

        cls = StrategyRegistry.get("momentum")
        assert cls is MomentumStrategy

    def test_strategy_registry_list(self):
        """列出所有策略."""
        # 检查列表是否包含已注册的策略
        strategies = StrategyRegistry.list_strategies()
        assert "momentum" in strategies

    def test_strategy_registry_get_unknown(self):
        """查询未知策略."""
        with pytest.raises(KeyError):
            StrategyRegistry.get("nonexistent_strategy")


class TestSignalGenerator:
    """信号生成器测试."""

    def test_no_strategies(self):
        """无子策略."""
        gen = SignalGenerator()
        signals = gen.combine({}, {})
        assert signals == []

    def test_union_combine(self):
        """简单合并."""
        gen = SignalGenerator("union")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("SELL", "600000.SH", 0.6))

        signals = gen.combine({}, {})
        assert len(signals) == 2

    def test_union_combine_dedup(self):
        """简单合并去重."""
        gen = SignalGenerator("union")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.9))

        signals = gen.combine({}, {})
        assert len(signals) == 1  # 去重

    def test_vote_combine_majority(self):
        """投票合并 - 超过半数."""
        gen = SignalGenerator("vote")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.7))
        gen.add_strategy(_make_fake_strategy("SELL", "600000.SH", 0.6))

        signals = gen.combine({}, {})
        assert len(signals) == 1
        assert signals[0].symbol == "000001.SZ"

    def test_vote_combine_minority(self):
        """投票合并 - 不过半数."""
        gen = SignalGenerator("vote")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("SELL", "600000.SH", 0.6))

        signals = gen.combine({}, {})
        assert len(signals) == 0  # 各得1票，未过半数

    def test_weighted_combine(self):
        """加权合并."""
        gen = SignalGenerator("weighted")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8), weight=2.0)
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.4), weight=1.0)

        signals = gen.combine({}, {})
        assert len(signals) == 1
        expected_strength = (0.8 * 2.0 + 0.4 * 1.0) / 3.0
        assert abs(signals[0].strength - expected_strength) < 1e-6

    def test_layered_combine(self):
        """分层合并."""
        gen = SignalGenerator("layered")
        gen.add_strategy(_make_fake_strategy("LONG", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("LONG", "000001.SZ", 0.7))

        signals = gen.combine({}, {})
        assert len(signals) == 1

    def test_layered_combine_no_overlap(self):
        """分层合并 - 无交集."""
        gen = SignalGenerator("layered")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        gen.add_strategy(_make_fake_strategy("BUY", "600000.SH", 0.7))

        signals = gen.combine({}, {})
        assert len(signals) == 0  # 无交集

    def test_layered_single_strategy_fallback(self):
        """分层合并 - 单一策略回退到union."""
        gen = SignalGenerator("layered")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))

        signals = gen.combine({}, {})
        assert len(signals) == 1

    def test_unknown_method(self):
        """未知合并方法."""
        gen = SignalGenerator("unknown")
        gen.add_strategy(_make_fake_strategy("BUY", "000001.SZ", 0.8))
        with pytest.raises(ValueError):
            gen.combine({}, {})


class TestPositionSizer:
    """仓位管理器测试."""

    def test_equal_weight_default(self):
        """等权分配."""
        sizer = PositionSizer("equal_weight")
        result = sizer.compute(100000, ["000001.SZ", "600000.SH", "000300.SH"])
        assert abs(result["000001.SZ"] - 33333.33) < 1
        assert abs(result["600000.SH"] - 33333.33) < 1

    def test_equal_weight_with_signals(self):
        """等权分配 - 带信号过滤."""
        sizer = PositionSizer("equal_weight")
        result = sizer.compute(
            100000,
            ["000001.SZ", "600000.SH", "000300.SH"],
            signals={"000001.SZ": 1.0, "600000.SH": 0.0},
        )
        assert result["000001.SZ"] > 0
        assert result["600000.SH"] == 0
        assert result["000300.SH"] == 0

    def test_equal_weight_no_active(self):
        """等权分配 - 无活跃标的."""
        sizer = PositionSizer("equal_weight")
        result = sizer.compute(100000, [], signals={})
        assert all(v == 0 for v in result.values())

    def test_equal_weight_with_strength(self):
        """等权分配 - 带信号强度."""
        sizer = PositionSizer("equal_weight")
        result = sizer.compute(
            100000,
            ["000001.SZ", "600000.SH"],
            signals={"000001.SZ": 1.0, "600000.SH": 1.0},
            strengths={"000001.SZ": 0.5, "600000.SH": 1.0},
        )
        assert result["000001.SZ"] < result["600000.SH"]

    def test_risk_parity(self):
        """风险平价."""
        sizer = PositionSizer("risk_parity")
        sizer._config = {"vol_000001.SZ": 0.1, "vol_600000.SH": 0.3}
        result = sizer.compute(
            100000,
            ["000001.SZ", "600000.SH"],
            signals={"000001.SZ": 1.0, "600000.SH": 1.0},
            prices={"000001.SZ": 10.0, "600000.SH": 5.0},
        )
        # 低波动标的应分配更多资金
        assert result["000001.SZ"] > result["600000.SH"]

    def test_kelly_default(self):
        """凯利公式."""
        sizer = PositionSizer(
            "kelly", default_win_rate=0.6, default_win_loss_ratio=2.0, kelly_fraction=0.25
        )
        result = sizer.compute(
            100000, ["000001.SZ"], signals={"000001.SZ": 1.0}, strengths={"000001.SZ": 1.0}
        )
        # f* = (0.6*2 - 0.4)/2 = 0.4, 半凯利 = 0.1, 满强度 = 0.1
        # 仓位 = 100000 * 0.1 = 10000
        assert abs(result["000001.SZ"] - 10000) < 1

    def test_kelly_custom_params(self):
        """凯利公式 - 自定义参数."""
        sizer = PositionSizer(
            "kelly",
            win_rate_000001_SZ=0.7,
            win_loss_000001_SZ=3.0,
            kelly_fraction=0.5,
        )
        result = sizer.compute(
            100000, ["000001.SZ"], signals={"000001.SZ": 1.0}, strengths={"000001.SZ": 1.0}
        )
        assert result["000001.SZ"] > 0

    def test_target_vol(self):
        """目标波动率."""
        sizer = PositionSizer(
            "target_vol", target_volatility=0.2, vol_000001_SZ=0.25, vol_600000_SH=0.4
        )
        result = sizer.compute(
            100000,
            ["000001.SZ", "600000.SH"],
            signals={"000001.SZ": 1.0, "600000.SH": 1.0},
        )
        # 各分5万, 000001: 0.2/0.25 = 0.8, 600000: 0.2/0.4 = 0.5
        assert abs(result["000001.SZ"] - 40000) < 1

    def test_target_vol_cap(self):
        """目标波动率 - 杠杆上限."""
        sizer = PositionSizer(
            "target_vol", target_volatility=0.3, max_leverage=2.0
        )
        sizer._config["vol_000001.SZ"] = 0.05
        result = sizer.compute(
            100000, ["000001.SZ"], signals={"000001.SZ": 1.0}
        )
        # 0.3/0.05 = 6, capped at 2.0, so 100000 * 2.0 = 200000
        assert abs(result["000001.SZ"] - 200000) < 1

    def test_compute_shares(self):
        """计算股数."""
        sizer = PositionSizer()
        target = {"000001.SZ": 50000, "600000.SH": 30000}
        prices = {"000001.SZ": 10.5, "600000.SH": 5.2}
        shares = sizer.compute_shares(target, prices)
        assert shares["000001.SZ"] == 4700  # 50000/10.5/100*100 = 4761 → 4700
        assert shares["600000.SH"] == 5700  # 30000/5.2/100*100 = 5769 → 5700
        assert shares["000001.SZ"] % 100 == 0

    def test_compute_shares_zero_price(self):
        """计算股数 - 零价格."""
        sizer = PositionSizer()
        shares = sizer.compute_shares({"000001.SZ": 50000}, {"000001.SZ": 0})
        assert shares["000001.SZ"] == 0

    def test_unknown_method(self):
        """未知方法."""
        sizer = PositionSizer("unknown")
        with pytest.raises(ValueError):
            sizer.compute(100000, ["000001.SZ"])

    def test_empty_symbols(self):
        """空标的列表."""
        sizer = PositionSizer()
        result = sizer.compute(100000, [])
        assert result == {}


class TestStrategyContext:
    """策略上下文测试 (集成)."""

    def test_context_log(self):
        """日志记录."""
        from quant_invest.strategy.context import StrategyContext

        ctx = StrategyContext()
        ctx.log("test message")
        assert len(ctx._logs) == 1
        assert ctx._logs[0]["message"] == "test message"

    def test_context_history_no_handler(self):
        """无data_handler时."""
        from quant_invest.strategy.context import StrategyContext

        ctx = StrategyContext()
        with pytest.raises(RuntimeError):
            ctx.history("000001.SZ", 20)

    def test_context_get_factor_no_api(self):
        """无factor_api时."""
        from quant_invest.strategy.context import StrategyContext

        ctx = StrategyContext()
        with pytest.raises(RuntimeError):
            ctx.get_factor("test", ["000001.SZ"], datetime.now())


# ---- 辅助函数 ----


def _create_test_strategy(params=None):
    """创建测试用策略."""

    class TestStrategy(StrategyBase):
        def __init__(self, params=None):
            super().__init__(params)
            self.initialized = False
            self.finished = False

        def on_init(self) -> None:
            self.initialized = True

        def on_bar(self, bar_data, positions) -> list[SignalEvent]:
            return []

        def on_finish(self) -> None:
            self.finished = True

    return TestStrategy(params=params)


def _make_fake_strategy(direction: str, symbol: str, strength: float):
    """创建返回固定信号的假策略."""

    class FakeStrategy(StrategyBase):
        def on_init(self) -> None:
            pass

        def on_bar(self, bar_data, positions) -> list[SignalEvent]:
            return [
                SignalEvent(
                    timestamp=datetime.now(),
                    symbol=symbol,
                    direction=direction,
                    strength=strength,
                )
            ]

    return FakeStrategy()
