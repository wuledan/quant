#!/usr/bin/env python3
"""BacktestEngine回测引擎测试."""

from __future__ import annotations

from datetime import date, datetime
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.backtest.engine import BacktestEngine, BacktestResult
from quant_invest.backtest.events import FillEvent, MarketEvent, OrderEvent, SignalEvent
from quant_invest.backtest.portfolio import Portfolio


class TestBacktestEngine:
    """BacktestEngine单元测试."""

    @pytest.fixture
    def mock_data_handler(self) -> MagicMock:
        """模拟DataHandler."""
        handler = MagicMock()
        dates = [datetime(2024, 1, 2), datetime(2024, 1, 3), datetime(2024, 1, 4)]
        events = [
            MarketEvent(timestamp=d, symbol="000001.SZ", bar_data={"000001.SZ": {"close": 10.0, "volume": 1_000_000}})
            for d in dates
        ]
        events.append(None)  # 最后一个返回None表示数据结束

        def side_effect():
            nonlocal events
            if events:
                return events.pop(0)
            return None

        handler.next_bar.side_effect = side_effect
        return handler

    @pytest.fixture
    def mock_broker(self) -> MagicMock:
        """模拟Broker."""
        broker = MagicMock()
        broker.execute_orders.return_value = []
        return broker

    @pytest.fixture
    def mock_strategy(self) -> MagicMock:
        """模拟Strategy."""
        strategy = MagicMock()
        strategy.on_init.return_value = None
        strategy.on_bar.return_value = []
        return strategy

    def test_engine_creation(self, mock_data_handler, mock_broker, mock_strategy):
        """测试引擎创建."""
        portfolio = Portfolio()
        engine = BacktestEngine(
            data_handler=mock_data_handler,
            broker=mock_broker,
            portfolio=portfolio,
            strategy=mock_strategy,
            initial_cash=1_000_000,
        )
        assert engine.initial_cash == 1_000_000
        assert engine.benchmark == "000300.SH"

    def test_engine_run(self, mock_data_handler, mock_broker, mock_strategy):
        """测试引擎运行主循环."""
        portfolio = Portfolio()
        engine = BacktestEngine(
            data_handler=mock_data_handler,
            broker=mock_broker,
            portfolio=portfolio,
            strategy=mock_strategy,
            initial_cash=1_000_000,
        )

        result = engine.run(
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 4),
        )

        assert isinstance(result, BacktestResult)
        assert result.initial_cash == 1_000_000
        mock_strategy.on_init.assert_called_once()
        assert mock_strategy.on_bar.call_count == 3

    def test_engine_run_with_signals(self, mock_data_handler, mock_strategy):
        """测试带信号的完整回测流程."""
        from quant_invest.backtest.broker import SimulatedBroker

        broker = SimulatedBroker()
        portfolio = Portfolio()

        # 策略生成买入信号
        mock_strategy.on_bar.return_value = [
            SignalEvent(
                timestamp=datetime(2024, 1, 2),
                symbol="000001.SZ",
                direction="LONG",
                strength=0.5,
            )
        ]

        engine = BacktestEngine(
            data_handler=mock_data_handler,
            broker=broker,
            portfolio=portfolio,
            strategy=mock_strategy,
            initial_cash=1_000_000,
        )

        # 使用更长的数据序列
        dates = [datetime(2024, 1, d) for d in range(2, 12)] + [None]
        mock_data_handler.next_bar.side_effect = [
            MarketEvent(
                timestamp=d if isinstance(d, datetime) else d,
                symbol="000001.SZ",
                bar_data={"000001.SZ": {"close": 10.0 + i * 0.1, "volume": 1_000_000 + i * 10000}},
            )
            if isinstance(d, datetime)
            else d
            for i, d in enumerate(dates)
        ]

        result = engine.run(
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 11),
        )

        assert isinstance(result, BacktestResult)
        assert result.total_trades >= 0

    def test_engine_empty_data(self, mock_strategy):
        """测试空数据的回测."""
        from quant_invest.backtest.broker import SimulatedBroker
        from quant_invest.backtest.data_handler import DailyDataHandler

        broker = SimulatedBroker()
        portfolio = Portfolio()

        handler = MagicMock()
        handler.next_bar.return_value = None

        engine = BacktestEngine(
            data_handler=handler,
            broker=broker,
            portfolio=portfolio,
            strategy=mock_strategy,
            initial_cash=1_000_000,
        )

        result = engine.run(
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 4),
        )

        assert result.final_value == 1_000_000
        assert result.total_return == 0.0

    def test_backtest_result_dataclass(self):
        """测试BacktestResult数据类."""
        result = BacktestResult(
            start_date=date(2024, 1, 2),
            end_date=date(2024, 12, 31),
            initial_cash=1_000_000,
            final_value=1_200_000,
            total_return=0.2,
            total_trades=50,
        )
        assert result.total_return == 0.2
        assert result.final_value == 1_200_000
        assert result.total_trades == 50
