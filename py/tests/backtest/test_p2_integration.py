#!/usr/bin/env python3
"""P2 集成测试 — 验证回测引擎 Python/C++ 模式一致性.

测试场景:
1. StorageEngineDataHandler 数据加载
2. Python 模式回测 vs C++ 执行模式回测
3. FactorEngineBridge 因子计算一致性
4. OrderAdapter 订单模拟一致性
"""

import sys
from datetime import date
from pathlib import Path

import pandas as pd
import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "py" / "src"))


class TestP2Integration:
    """P2 集成测试."""

    def test_storage_engine_data_handler(self) -> None:
        """测试 StorageEngineDataHandler 数据加载."""
        from quant_invest.backtest.data_handler import StorageEngineDataHandler

        handler = StorageEngineDataHandler(
            symbols=["000001.SH", "600519.SH"],
            start_date=date(2025, 1, 1),
            end_date=date(2025, 12, 31),
            data_dir=str((PROJECT_ROOT / "py" / "data" / "daily").resolve()),
        )

        assert len(handler._dates) > 0, "应有日期数据"
        assert "000001.SH" in handler._data, "应有 000001.SH 数据"

        # 获取第一根K线
        event = handler.next_bar()
        assert event is not None, "应返回事件"
        assert len(event.bar_data) > 0, "应有K线数据"

        # 获取当前K线
        bar = handler.current_bar("000001.SH")
        assert bar is not None, "应返回当前K线"
        assert "close" in bar, "应包含 close 字段"

    def test_daily_data_handler_vs_storage_handler(self) -> None:
        """测试 DailyDataHandler 和 StorageEngineDataHandler 数据一致性."""
        from quant_invest.backtest.data_handler import DailyDataHandler, StorageEngineDataHandler
        from quant_invest.data.providers import DataProviderFactory

        symbols = ["000001.SH"]
        start = date(2025, 5, 20)
        end = date(2025, 12, 31)

        provider = DataProviderFactory.create("yahoo")

        # Python 模式
        py_handler = DailyDataHandler(
            symbols=symbols,
            start_date=start,
            end_date=end,
            data_provider=provider,
        )

        # C++ StorageEngine 模式
        cpp_handler = StorageEngineDataHandler(
            symbols=symbols,
            start_date=start,
            end_date=end,
            data_dir=str((PROJECT_ROOT / "py" / "data" / "daily").resolve()),
        )

        # 对比数据内容（不对比日期数，因为数据源可能不同）
        for symbol in symbols:
            py_df = py_handler._data.get(symbol, pd.DataFrame())
            cpp_df = cpp_handler._data.get(symbol, pd.DataFrame())
            if py_df.empty or cpp_df.empty:
                continue
            # 对比重叠日期的收盘价
            common_idx = py_df.index.intersection(cpp_df.index)
            if len(common_idx) > 0:
                py_common = py_df.loc[common_idx, "close"]
                cpp_common = cpp_df.loc[common_idx, "close"]
                max_diff = (py_common - cpp_common).abs().max()
                assert max_diff < 0.01, f"{symbol}: 重叠日期收盘价应一致, max_diff={max_diff}"

        # 对比数据内容
        for symbol in symbols:
            py_df = py_handler._data.get(symbol, pd.DataFrame())
            cpp_df = cpp_handler._data.get(symbol, pd.DataFrame())
            if py_df.empty or cpp_df.empty:
                continue
            assert abs(len(py_df) - len(cpp_df)) <= 2, f"{symbol}: 行数应接近"
            # 最新收盘价应一致
            py_close = py_df["close"].iloc[-1]
            cpp_close = cpp_df["close"].iloc[-1]
            assert abs(py_close - cpp_close) < 0.01, f"{symbol}: 收盘价应一致"

    def test_order_adapter_python_mode(self) -> None:
        """测试 OrderAdapter 纯 Python 模式."""
        from quant_invest.execution.order_adapter import OrderAdapter

        adapter = OrderAdapter()
        # 如果 C++ 可用，手动切换到 Python 模式测试
        adapter._cpp_available = False
        adapter.connect_broker()

        # 提交市价单
        result = adapter.submit_order(
            symbol="000001.SZ",
            side="BUY",
            order_type="MARKET",
            price=10.0,
            quantity=1000,
        )
        assert result.success, "市价单应成功"
        assert result.filled_qty == 1000, "市价单应全部成交"

        # 提交限价单
        result2 = adapter.submit_order(
            symbol="600519.SH",
            side="SELL",
            order_type="LIMIT",
            price=1800.0,
            quantity=50,
        )
        assert result2.order_id is not None, "限价单应有 order_id"

        # 模拟成交
        fill = adapter.simulate_fill(result2.order_id, 1805.0, 50)
        assert fill.status == "FILLED", "限价单应全部成交"
        assert fill.filled_qty == 50, "成交数量应正确"

    def test_order_adapter_cpp_mode(self) -> None:
        """测试 OrderAdapter C++ 模式."""
        from quant_invest.execution.order_adapter import OrderAdapter

        adapter = OrderAdapter()
        if not adapter.cpp_available:
            pytest.skip("C++ 执行引擎不可用")

        adapter.connect_broker()

        # 提交市价单
        result = adapter.submit_order(
            symbol="000001.SZ",
            side="BUY",
            order_type="MARKET",
            price=10.0,
            quantity=1000,
        )
        assert result.success, "市价单应成功"
        assert result.filled_qty == 1000, "市价单应全部成交"

        # 提交限价单
        result2 = adapter.submit_order(
            symbol="600519.SH",
            side="SELL",
            order_type="LIMIT",
            price=1800.0,
            quantity=50,
        )
        assert result2.order_id is not None, "限价单应有 order_id"

        # 模拟成交
        fill = adapter.simulate_fill(result2.order_id, 1805.0, 50)
        assert fill.status == "FILLED", "限价单应全部成交"

        adapter.disconnect_broker()

    def test_factor_engine_bridge(self) -> None:
        """测试 FactorEngineBridge 因子计算."""
        from quant_invest.strategy.factor_engine import FactorEngineBridge
        from quant_invest.strategy.dsl import Strategy, Factor, cross_above

        class MAStrategy(Strategy):
            fast_ma = Factor(kind="MA", input="close", params={"period": 5})
            slow_ma = Factor(kind="MA", input="close", params={"period": 20})
            golden_cross = cross_above(fast_ma, slow_ma)

        strategy = MAStrategy()
        bridge = FactorEngineBridge(strategy)
        bridge.initialize()

        # 模拟 K 线
        prices = [10.0, 10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 12.8, 12.5, 12.0,
                  11.5, 11.0, 10.5, 10.0, 9.5, 9.0, 8.5, 8.0, 7.5, 7.0,
                  8.0, 9.0, 10.0, 11.0, 12.0]

        for price in prices:
            result = bridge.on_bar("TEST", {"close": price})

        # 因子值应已计算
        assert result.factor_values is not None, "因子值不应为 None"
        assert "fast_ma" in result.factor_values, "应有 fast_ma 因子"
        assert "slow_ma" in result.factor_values, "应有 slow_ma 因子"

    def test_backtest_engine_python_mode(self) -> None:
        """测试 BacktestEngine Python 模式回测."""
        from quant_invest.backtest.engine import BacktestEngine
        from quant_invest.backtest.broker import SimulatedBroker
        from quant_invest.backtest.portfolio import Portfolio
        from quant_invest.backtest.data_handler import StorageEngineDataHandler
        from quant_invest.strategy.base import StrategyBase

        class NoOpStrategy(StrategyBase):
            def on_init(self, ctx) -> None:
                pass

            def on_bar(self, bar_dataframes, positions) -> None:
                pass

        handler = StorageEngineDataHandler(
            symbols=["000001.SH"],
            start_date=date(2025, 5, 20),
            end_date=date(2025, 12, 31),
            data_dir=str((PROJECT_ROOT / "py" / "data" / "daily").resolve()),
        )
        broker = SimulatedBroker()
        portfolio = Portfolio()

        engine = BacktestEngine(
            data_handler=handler,
            broker=broker,
            portfolio=portfolio,
            strategy=NoOpStrategy(),
            initial_cash=1_000_000.0,
        )

        result = engine.run(
            start_date=date(2025, 5, 20),
            end_date=date(2025, 12, 31),
        )
        assert result is not None, "回测应返回结果"