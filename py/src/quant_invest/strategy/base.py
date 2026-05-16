"""策略基类

生命周期：
__init__ -> on_init -> (on_bar 循环) -> on_finish
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Any

from ..backtest.events import FillEvent, SignalEvent


@dataclass
class StrategyParams:
    """策略参数基类"""

    pass


class StrategyBase(ABC):
    """策略基类

    子类需实现：
    - on_init(): 初始化
    - on_bar(): 每根K线触发的信号生成逻辑
    - on_finish(): 收尾（可选）
    """

    def __init__(self, params: StrategyParams | None = None) -> None:
        self.params = params or StrategyParams()
        self._context: Any | None = None

    @abstractmethod
    def on_init(self) -> None:
        """策略初始化"""
        ...

    @abstractmethod
    def on_bar(self, bar_data: dict, positions: dict) -> list[SignalEvent]:
        """每根K线触发"""
        ...

    def on_finish(self) -> None:
        """策略结束（可选重写）"""
        pass

    def on_order_filled(self, fill: FillEvent) -> None:
        """订单成交回调（可选重写）"""
        pass

    @property
    def context(self) -> Any:
        """策略上下文：访问数据、因子、日志等"""
        assert self._context is not None, "Context not initialized"
        return self._context

    def set_context(self, context: Any) -> None:
        self._context = context
