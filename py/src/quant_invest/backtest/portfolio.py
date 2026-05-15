"""持仓管理"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

import pandas as pd

from .events import FillEvent, MarketEvent, OrderEvent, SignalEvent


@dataclass
class Position:
    """单个持仓"""

    symbol: str
    quantity: int = 0
    avg_price: float = 0.0
    market_value: float = 0.0
    unrealized_pnl: float = 0.0


class Portfolio:
    """组合管理器

    负责：
    1. 根据策略信号计算目标仓位
    2. 生成调仓订单（目标仓位 - 当前仓位）
    3. 维护持仓和净值记录
    """

    def __init__(self) -> None:
        self.initial_cash: float = 0.0
        self.cash: float = 0.0
        self.positions: dict[str, Position] = {}
        self._nav_history: list[dict] = []

    def initialize(self, initial_cash: float) -> None:
        """初始化组合"""
        self.initial_cash = initial_cash
        self.cash = initial_cash

    @property
    def current_positions(self) -> dict[str, Position]:
        """当前持仓"""
        return self.positions

    @property
    def total_value(self) -> float:
        """总资产 = 现金 + 持仓市值"""
        return self.cash + sum(p.market_value for p in self.positions.values())

    def generate_orders(
        self,
        signals: list[SignalEvent],
        market_event: MarketEvent,
    ) -> list[OrderEvent]:
        """根据信号和当前持仓生成订单"""
        # TODO: 实现
        raise NotImplementedError("Portfolio.generate_orders not implemented")

    def update_from_fills(self, fills: list[FillEvent], timestamp: datetime) -> None:
        """根据成交结果更新持仓"""
        # TODO: 实现
        raise NotImplementedError("Portfolio.update_from_fills not implemented")

    def record_snapshot(self, timestamp: datetime) -> None:
        """记录当前组合快照"""
        # TODO: 实现
        raise NotImplementedError("Portfolio.record_snapshot not implemented")

    def get_nav_dataframe(self) -> pd.DataFrame:
        """获取净值曲线DataFrame"""
        # TODO: 实现
        raise NotImplementedError("Portfolio.get_nav_dataframe not implemented")
