"""SimulatedBroker模拟券商测试."""

from __future__ import annotations

from datetime import datetime

from quant_invest.backtest.broker import (
    CommissionConfig,
    RestrictionConfig,
    SimulatedBroker,
)
from quant_invest.backtest.events import MarketEvent, OrderEvent


class TestSimulatedBroker:
    """SimulatedBroker单元测试."""

    def test_broker_default_config(self):
        """默认配置测试."""
        broker = SimulatedBroker()
        assert broker.commission_config.commission_rate == 0.00025
        assert broker.commission_config.stamp_tax_rate == 0.001
        assert broker.commission_config.min_commission == 5.0
        assert broker.restriction_config.limit_up_ratio == 0.1

    def test_execute_buy_order(self):
        """买入订单执行."""
        broker = SimulatedBroker()
        order = OrderEvent(
            timestamp=datetime(2024, 1, 2),
            symbol="000001.SZ",
            direction="BUY",
            quantity=1000,
            price=10.0,
        )
        market_event = MarketEvent(
            timestamp=datetime(2024, 1, 2),
            bar_data={"000001.SZ": {"close": 10.0, "volume": 1_000_000}},
        )

        fills = broker.execute_orders([order], market_event)
        assert len(fills) == 1
        assert fills[0].symbol == "000001.SZ"
        assert fills[0].direction == "BUY"
        assert fills[0].quantity == 1000
        assert fills[0].fill_price > 0
        assert fills[0].commission > 0

    def test_execute_sell_order(self):
        """卖出订单执行."""
        broker = SimulatedBroker()
        order = OrderEvent(
            timestamp=datetime(2024, 1, 2),
            symbol="000001.SZ",
            direction="SELL",
            quantity=500,
            price=10.5,
        )
        market_event = MarketEvent(
            timestamp=datetime(2024, 1, 2),
            bar_data={"000001.SZ": {"close": 10.5, "volume": 2_000_000}},
        )

        fills = broker.execute_orders([order], market_event)
        assert len(fills) == 1
        assert fills[0].direction == "SELL"
        # 卖出需缴印花税
        assert fills[0].commission > 0.025

    def test_buy_at_limit_up_blocked(self):
        """涨停时买入被阻止."""
        broker = SimulatedBroker()
        order = OrderEvent(
            timestamp=datetime(2024, 1, 2),
            symbol="000001.SZ",
            direction="BUY",
            quantity=1000,
        )
        market_event = MarketEvent(
            timestamp=datetime(2024, 1, 2),
            bar_data={
                "000001.SZ": {
                    "close": 11.0,  # 涨停价
                    "prev_close": 10.0,  # 前收盘
                    "volume": 1_000_000,
                }
            },
        )

        fills = broker.execute_orders([order], market_event)
        assert len(fills) == 0

    def test_sell_at_limit_down_blocked(self):
        """跌停时卖出被阻止."""
        broker = SimulatedBroker()
        order = OrderEvent(
            timestamp=datetime(2024, 1, 2),
            symbol="000001.SZ",
            direction="SELL",
            quantity=1000,
        )
        market_event = MarketEvent(
            timestamp=datetime(2024, 1, 2),
            bar_data={
                "000001.SZ": {
                    "close": 9.0,  # 跌停价
                    "prev_close": 10.0,  # 前收盘
                    "volume": 1_000_000,
                }
            },
        )

        fills = broker.execute_orders([order], market_event)
        assert len(fills) == 0

    def test_empty_orders(self):
        """空订单列表."""
        broker = SimulatedBroker()
        market_event = MarketEvent(timestamp=datetime(2024, 1, 2))

        fills = broker.execute_orders([], market_event)
        assert len(fills) == 0

    def test_commission_config_custom(self):
        """自定义手续费配置."""
        config = CommissionConfig(
            commission_rate=0.0005,
            min_commission=1.0,
            stamp_tax_rate=0.0,
        )
        broker = SimulatedBroker(commission_config=config)
        assert broker.commission_config.commission_rate == 0.0005

    def test_restriction_disabled(self):
        """禁用涨跌停限制."""
        config = RestrictionConfig(check_limit=False)
        broker = SimulatedBroker(restriction_config=config)
        order = OrderEvent(
            timestamp=datetime(2024, 1, 2),
            symbol="000001.SZ",
            direction="BUY",
            quantity=1000,
        )
        market_event = MarketEvent(
            timestamp=datetime(2024, 1, 2),
            bar_data={
                "000001.SZ": {
                    "close": 11.0,
                    "prev_close": 10.0,
                    "volume": 1_000_000,
                }
            },
        )

        fills = broker.execute_orders([order], market_event)
        assert len(fills) == 1

    def test_calculate_buy_commission(self):
        """计算买入手续费."""
        broker = SimulatedBroker()
        order = OrderEvent(timestamp=datetime(2024, 1, 2), symbol="000001.SZ", direction="BUY", quantity=1000, price=10.0)
        commission = broker._calculate_commission(order, 10.0)
        # 买入: 佣金 10000 * 0.00025 = 2.5, 不满5按5算
        assert commission == 5.0

    def test_calculate_sell_commission(self):
        """计算卖出手续费（含印花税）."""
        broker = SimulatedBroker()
        order = OrderEvent(timestamp=datetime(2024, 1, 2), symbol="000001.SZ", direction="SELL", quantity=10000, price=10.0)
        commission = broker._calculate_commission(order, 10.0)
        # 卖出: 佣金 100000 * 0.00025 = 25 + 印花税 100000 * 0.001 = 100 = 125
        assert commission == 125.0
