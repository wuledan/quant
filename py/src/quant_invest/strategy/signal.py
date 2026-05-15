"""信号生成与组合

支持多个子策略的信号合并：
1. 简单合并：所有子策略信号的并集
2. 投票合并：超过N个子策略同意才产生信号
3. 加权合并：按子策略历史表现加权
4. 分层合并：先用因子信号过滤，再用择时信号确认
"""

from __future__ import annotations

from ..backtest.events import SignalEvent
from .base import StrategyBase


class SignalGenerator:
    """信号生成与组合"""

    def __init__(self, method: str = "union") -> None:
        self.method = method
        self._sub_strategies: list[StrategyBase] = []
        self._weights: list[float] = []

    def add_strategy(self, strategy: StrategyBase, weight: float = 1.0) -> None:
        self._sub_strategies.append(strategy)
        self._weights.append(weight)

    def combine(self, bar_data: dict, positions: dict) -> list[SignalEvent]:
        """合并多个子策略信号"""
        # TODO: 实现
        raise NotImplementedError("SignalGenerator.combine not implemented")
