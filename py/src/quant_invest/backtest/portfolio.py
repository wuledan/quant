#!/usr/bin/env python3
"""持仓管理"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Any

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
        self._nav_history: list[dict[str, Any]] = []

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
        """根据信号和当前持仓生成订单.

        LONG 信号：如果未持仓，则买入
        SHORT 信号：如果已持仓，则卖出
        EXIT 信号：如果已持仓，则清仓
        """
        orders: list[OrderEvent] = []
        bar_data = market_event.bar_data or {}

        for signal in signals:
            current_pos = self.positions.get(signal.symbol)
            current_qty = current_pos.quantity if current_pos else 0

            if signal.direction == "LONG" and current_qty <= 0:
                # 买入
                symbol_data = bar_data.get(signal.symbol, {})
                price = (
                    symbol_data.get("close", signal.price) if isinstance(symbol_data, dict) else signal.price
                )
                # 按信号强度决定仓位比例
                strength = max(0.0, min(1.0, signal.strength))
                cash_per_symbol = self.cash * 0.95  # 留5%现金
                quantity = int(cash_per_symbol * strength / price) if price > 0 else 0

                if quantity > 0:
                    orders.append(
                        OrderEvent(
                            timestamp=signal.timestamp,
                            symbol=signal.symbol,
                            direction="BUY",
                            quantity=quantity,
                            price=price,
                            order_type="MARKET",
                        )
                    )

            elif signal.direction == "SHORT" and current_qty > 0:
                # 卖出
                orders.append(
                    OrderEvent(
                        timestamp=signal.timestamp,
                        symbol=signal.symbol,
                        direction="SELL",
                        quantity=current_qty,
                        price=0.0,
                        order_type="MARKET",
                    )
                )

            elif signal.direction == "EXIT" and current_qty > 0:
                orders.append(
                    OrderEvent(
                        timestamp=signal.timestamp,
                        symbol=signal.symbol,
                        direction="SELL",
                        quantity=current_qty,
                        price=0.0,
                        order_type="MARKET",
                    )
                )

        return orders

    def update_from_fills(self, fills: list[FillEvent], timestamp: datetime) -> None:
        """根据成交结果更新持仓"""
        for fill in fills:
            if fill.direction == "BUY":
                # 买入
                if fill.symbol in self.positions:
                    pos = self.positions[fill.symbol]
                    total_cost = pos.avg_price * pos.quantity + fill.fill_price * fill.quantity
                    pos.quantity += fill.quantity
                    pos.avg_price = total_cost / pos.quantity if pos.quantity > 0 else 0.0
                else:
                    self.positions[fill.symbol] = Position(
                        symbol=fill.symbol,
                        quantity=fill.quantity,
                        avg_price=fill.fill_price,
                        market_value=fill.fill_price * fill.quantity,
                    )

                self.cash -= fill.fill_price * fill.quantity + fill.commission

            elif fill.direction == "SELL":
                # 卖出
                pos = self.positions.get(fill.symbol)
                if pos:
                    pos.quantity -= fill.quantity
                    if pos.quantity <= 0:
                        del self.positions[fill.symbol]
                    else:
                        pos.market_value = pos.avg_price * pos.quantity

                self.cash += fill.fill_price * fill.quantity - fill.commission

            # 更新持仓市值
            for pos in self.positions.values():
                # 使用成交价近似市值
                pos.market_value = pos.avg_price * pos.quantity
                pos.unrealized_pnl = 0.0  # 重新计算需参考市价

    def record_snapshot(self, timestamp: datetime) -> None:
        """记录当前组合快照（用于后续绩效分析）"""
        self._nav_history.append(
            {
                "timestamp": timestamp,
                "total_value": self.total_value,
                "cash": self.cash,
                "positions_value": sum(p.market_value for p in self.positions.values()),
                "positions_count": len(self.positions),
            }
        )

    def get_nav_dataframe(self) -> pd.DataFrame:
        """获取净值曲线DataFrame"""
        if not self._nav_history:
            return pd.DataFrame()

        df = pd.DataFrame(self._nav_history)
        df = df.set_index("timestamp")
        df.index.name = "date"
        return df
