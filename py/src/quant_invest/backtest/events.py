"""回测事件定义."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from enum import Enum, auto


class EventType(Enum):
    """事件类型."""

    MARKET = auto()
    SIGNAL = auto()
    ORDER = auto()
    FILL = auto()
    TIMER = auto()


@dataclass
class Event:
    """事件基类."""

    type: EventType
    timestamp: datetime


@dataclass
class MarketEvent:
    """市场数据事件：新的K线/Tick到达."""

    timestamp: datetime
    symbol: str = ""
    bar_data: dict | None = None
    type: EventType = EventType.MARKET


@dataclass
class SignalEvent:
    """策略信号事件."""

    timestamp: datetime
    symbol: str = ""
    direction: str = ""  # "LONG" / "SHORT" / "EXIT"
    strength: float = 1.0
    price: float = 0.0
    reason: str = ""
    type: EventType = EventType.SIGNAL


@dataclass
class OrderEvent:
    """订单事件."""

    timestamp: datetime
    symbol: str = ""
    order_type: str = "MARKET"  # MARKET / LIMIT / STOP
    quantity: int = 0
    direction: str = "BUY"
    price: float = 0.0
    order_id: str = ""
    type: EventType = EventType.ORDER


@dataclass
class FillEvent:
    """成交事件."""

    timestamp: datetime
    symbol: str = ""
    direction: str = "BUY"
    quantity: int = 0
    fill_price: float = 0.0
    commission: float = 0.0
    slippage: float = 0.0
    order_id: str = ""
    type: EventType = EventType.FILL
