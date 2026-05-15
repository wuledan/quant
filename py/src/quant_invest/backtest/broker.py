"""模拟经纪商

模拟真实交易的各个方面：
- 滑点：按比例/固定/成交量加权
- 手续费：印花税(卖)+券商佣金(双向)
- 涨跌停：涨停无法买入，跌停无法卖出
- 成交量限制：订单量不超过当前bar成交量的可占比例
"""

from __future__ import annotations

from dataclasses import dataclass

from .events import FillEvent, MarketEvent, OrderEvent


@dataclass
class CommissionConfig:
    """手续费配置（A股规则）"""

    stamp_tax_rate: float = 0.001  # 印花税：卖出千分之一
    commission_rate: float = 0.00025  # 券商佣金：万2.5
    min_commission: float = 5.0  # 最低佣金5元
    slippage_rate: float = 0.001  # 滑点率
    slippage_model: str = "percentage"  # "percentage" / "fixed" / "volume_weighted"


@dataclass
class RestrictionConfig:
    """涨跌停限制配置"""

    limit_up_ratio: float = 0.1  # 涨停比例 10%
    limit_down_ratio: float = -0.1  # 跌停比例 -10%
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
        # TODO: 实现完整的模拟执行逻辑
        raise NotImplementedError("SimulatedBroker._execute_single not implemented")

    def _apply_slippage(self, order: OrderEvent, market_event: MarketEvent) -> float:
        """应用滑点模型"""
        # TODO: 实现
        raise NotImplementedError("SimulatedBroker._apply_slippage not implemented")

    def _is_restricted(self, order: OrderEvent, market_event: MarketEvent) -> bool:
        """检查涨跌停限制"""
        # TODO: 实现
        raise NotImplementedError("SimulatedBroker._is_restricted not implemented")

    def _calculate_commission(self, order: OrderEvent, fill_price: float) -> float:
        """计算手续费（印花税 + 券商佣金）"""
        # TODO: 实现
        raise NotImplementedError("SimulatedBroker._calculate_commission not implemented")
