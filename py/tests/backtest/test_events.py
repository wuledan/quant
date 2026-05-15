"""回测事件引擎测试."""

from __future__ import annotations

from datetime import datetime

from quant_invest.backtest.events import EventType, FillEvent, MarketEvent, OrderEvent, SignalEvent


class TestEvents:
    """事件定义测试."""

    def test_market_event_creation(self):
        """MarketEvent创建."""
        event = MarketEvent(
            timestamp=datetime(2024, 1, 2, 9, 30),
            symbol="000001.SZ",
            bar_data={"open": 10.0, "close": 10.5},
        )
        assert event.type == EventType.MARKET
        assert event.symbol == "000001.SZ"
        assert event.bar_data["close"] == 10.5

    def test_signal_event_creation(self):
        """SignalEvent创建."""
        event = SignalEvent(
            timestamp=datetime(2024, 1, 2, 9, 35),
            symbol="000001.SZ",
            direction="LONG",
            strength=0.8,
        )
        assert event.type == EventType.SIGNAL
        assert event.direction == "LONG"
        assert event.strength == 0.8

    def test_order_event_creation(self):
        """OrderEvent创建."""
        event = OrderEvent(
            timestamp=datetime(2024, 1, 2, 9, 35),
            symbol="000001.SZ",
            direction="BUY",
            quantity=100,
        )
        assert event.type == EventType.ORDER
        assert event.quantity == 100

    def test_fill_event_creation(self):
        """FillEvent创建."""
        event = FillEvent(
            timestamp=datetime(2024, 1, 2, 9, 35),
            symbol="000001.SZ",
            direction="BUY",
            quantity=100,
            fill_price=10.5,
            commission=2.625,
        )
        assert event.type == EventType.FILL
        assert event.fill_price == 10.5
        assert event.commission == 2.625

    def test_event_type_enum(self):
        """EventType枚举值."""
        assert EventType.MARKET != EventType.SIGNAL
        assert EventType.ORDER != EventType.FILL
