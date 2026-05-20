"""回测框架"""

from .broker import CommissionConfig, RestrictionConfig, SimulatedBroker
from .data_handler import DailyDataHandler, StorageEngineDataHandler
from .engine import BacktestEngine, BacktestResult
from .events import Event, EventType, FillEvent, MarketEvent, OrderEvent, SignalEvent
from .performance import PerformanceAnalyzer, PerformanceMetrics
from .portfolio import Portfolio, Position

__all__ = [
    "BacktestEngine",
    "BacktestResult",
    "DailyDataHandler",
    "StorageEngineDataHandler",
    "Event",
    "EventType",
    "MarketEvent",
    "SignalEvent",
    "OrderEvent",
    "FillEvent",
    "SimulatedBroker",
    "CommissionConfig",
    "RestrictionConfig",
    "Portfolio",
    "Position",
    "PerformanceAnalyzer",
    "PerformanceMetrics",
]
