#!/usr/bin/env python3
"""模拟经纪商

模拟真实交易的各个方面：
- 滑点：按比例/固定/成交量加权
- 手续费：印花税(卖)+券商佣金(双向)
- 涨跌停：涨停无法买入，跌停无法卖出
- 成交量限制：订单量不超过当前bar成交量的可占比例
"""

from __future__ import annotations

from dataclasses import dataclass

from .commission import calc_a_share_commission
from .events import FillEvent, MarketEvent, OrderEvent
from .restrictions import is_at_limit_down, is_at_limit_up
from .slippage import fixed_slippage, percentage_slippage


@dataclass
class CommissionConfig:
    """手续费配置（A股规则）"""

    stamp_tax_rate: float = 0.001
    commission_rate: float = 0.00025
    min_commission: float = 5.0
    slippage_rate: float = 0.001
    slippage_model: str = "percentage"


@dataclass
class RestrictionConfig:
    """涨跌停限制配置"""

    limit_up_ratio: float = 0.1
    limit_down_ratio: float = -0.1
    check_limit: bool = True
    block_buy_at_limit_up: bool = True
    block_sell_at_limit_down: bool = True


class SimulatedBroker:
    """模拟经纪商"""

    def __init__(
        self,
        commission_config: CommissionConfig | None = None,
        restriction_config: RestrictionConfig | None = None,
    ) -> None:
        self.commission_config = commission_config or CommissionConfig()
        self.restriction_config = restriction_config or RestrictionConfig()

    def execute_orders(
        self,
        orders: list[OrderEvent],
        market_event: MarketEvent,
    ) -> list[FillEvent]:
        """执行订单列表，返回成交结果"""
        fills = []
        for order in orders:
            fill = self._execute_single(order, market_event)
            if fill is not None:
                fills.append(fill)
        return fills

    def _execute_single(
        self,
        order: OrderEvent,
        market_event: MarketEvent,
    ) -> FillEvent | None:
        """执行单个订单"""
        # 1. 计算滑点价格
        fill_price = self._apply_slippage(order, market_event)

        # 2. 检查涨跌停限制
        if self._is_restricted(order, market_event):
            return None

        # 3. 计算手续费
        commission = self._calculate_commission(order, fill_price)

        # 4. 成交量限制
        bar_data = market_event.bar_data or {}
        symbol_data = bar_data.get(order.symbol, {})
        if isinstance(symbol_data, dict) and "volume" in symbol_data:
            max_volume_ratio = self.commission_config.slippage_rate
            bar_volume = symbol_data["volume"]
            max_fill_qty = int(bar_volume * max_volume_ratio) if bar_volume > 0 else 0
            actual_qty = min(order.quantity, max_fill_qty) if max_fill_qty > 0 else order.quantity
        else:
            actual_qty = order.quantity

        if actual_qty <= 0:
            return None

        return FillEvent(
            timestamp=order.timestamp,
            symbol=order.symbol,
            direction=order.direction,
            quantity=actual_qty,
            fill_price=fill_price,
            commission=commission,
            slippage=abs(fill_price - order.price) if order.price > 0 else 0.0,
            order_id=order.order_id,
        )

    def _apply_slippage(self, order: OrderEvent, market_event: MarketEvent) -> float:
        """应用滑点模型"""
        model = self.commission_config.slippage_model
        rate = self.commission_config.slippage_rate

        bar_data = market_event.bar_data or {}
        symbol_data = bar_data.get(order.symbol, {})
        reference_price = (
            symbol_data.get("close", order.price)
            if isinstance(symbol_data, dict)
            else order.price
        )

        if order.price > 0:
            reference_price = order.price

        if model == "fixed":
            return fixed_slippage(reference_price, rate, order.direction)
        else:
            return percentage_slippage(reference_price, rate, order.direction)

    def _is_restricted(self, order: OrderEvent, market_event: MarketEvent) -> bool:
        """检查涨跌停限制"""
        config = self.restriction_config
        if not config.check_limit:
            return False

        bar_data = market_event.bar_data or {}
        symbol_data = bar_data.get(order.symbol, {})
        if not isinstance(symbol_data, dict):
            return False

        close_price = symbol_data.get("close", 0.0)
        if close_price <= 0:
            return False

        prev_close = symbol_data.get("prev_close", close_price)

        if order.direction == "BUY" and config.block_buy_at_limit_up:
            if is_at_limit_up(close_price, prev_close, config.limit_up_ratio):
                return True

        if order.direction == "SELL" and config.block_sell_at_limit_down:
            if is_at_limit_down(close_price, prev_close, config.limit_down_ratio):
                return True

        return False

    def _calculate_commission(self, order: OrderEvent, fill_price: float) -> float:
        """计算手续费（印花税 + 券商佣金）"""
        return calc_a_share_commission(
            price=fill_price,
            quantity=order.quantity,
            direction=order.direction,
            commission_rate=self.commission_config.commission_rate,
            min_commission=self.commission_config.min_commission,
            stamp_tax_rate=self.commission_config.stamp_tax_rate,
        )
