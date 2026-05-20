#!/usr/bin/env python3
"""策略声明式 DSL — 声明因子、信号组合、策略注册.

设计目标:
1. 声明式定义因子和信号，编译器可据此构建 FactorDAG
2. 支持 cross_above / cross_below 等信号组合器
3. 与现有 on_bar 策略向后兼容
4. 每个 Factor / SignalExpr 节点追踪依赖关系，供 DAG 编译器使用

用法示例::

    from quant_invest.strategy.dsl import Strategy, Factor, cross_above, strategy

    @strategy("ma_cross")
    class MACross(Strategy):
        fast_ma = Factor("SMA", period=5, input="close")
        slow_ma = Factor("SMA", period=20, input="close")
        signal = cross_above(fast_ma, slow_ma)

        def on_signal(self, ctx):
            if self.signal > 0:
                ctx.order(symbol=ctx.symbol, side="BUY",
                          quantity=ctx.cash * 0.95 / ctx.price)
            elif self.signal < 0:
                ctx.order(symbol=ctx.symbol, side="SELL",
                          quantity=ctx.position)
"""

from __future__ import annotations

import uuid
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any

from .registry import StrategyRegistry


# ---------------------------------------------------------------------------
# DAG 节点基类
# ---------------------------------------------------------------------------

class DAGNode:
    """FactorDAG 节点基类 — 所有可追踪依赖的表达式节点.

    每个 DAGNode 拥有唯一 id，编译器据此构建有向无环图。
    子类需实现 ``dependencies()`` 返回上游节点列表。
    """

    def __init__(self) -> None:
        self._node_id: str = uuid.uuid4().hex[:12]

    @property
    def node_id(self) -> str:
        return self._node_id

    @abstractmethod
    def dependencies(self) -> list[DAGNode]:
        """返回当前节点的上游依赖节点."""
        ...

    def walk(self) -> list[DAGNode]:
        """深度优先遍历，返回拓扑序节点列表（自身最后）."""
        visited: set[str] = set()
        result: list[DAGNode] = []

        def _dfs(node: DAGNode) -> None:
            if node.node_id in visited:
                return
            visited.add(node.node_id)
            for dep in node.dependencies():
                _dfs(dep)
            result.append(node)

        _dfs(self)
        return result


# ---------------------------------------------------------------------------
# Factor — 因子声明
# ---------------------------------------------------------------------------

class Factor(DAGNode):
    """声明一个因子计算节点.

    Args:
        kind: 因子类型名，对应 C++ FactorDAG 中注册的算子
              (如 "SMA", "EMA", "RSI", "MACD", "BBANDS" 等)
        input: 输入数据列名 (如 "close", "open", "high", "low", "volume")
        **params: 因子参数 (如 period=20, fast=12, slow=26)

    示例::

        fast_ma = Factor("SMA", period=5, input="close")
        rsi_14  = Factor("RSI", period=14, input="close")
    """

    def __init__(self, kind: str, *, input: str = "close", **params: Any) -> None:
        super().__init__()
        self.kind = kind
        self.input = input
        self.params: dict[str, Any] = dict(params)

    def dependencies(self) -> list[DAGNode]:
        """Factor 是叶子节点，无上游依赖（输入来自原始行情数据）."""
        return []

    def __repr__(self) -> str:
        param_str = ", ".join(f"{k}={v}" for k, v in self.params.items())
        parts = [f"Factor({self.kind!r}"]
        if self.input != "close":
            parts.append(f", input={self.input!r}")
        if param_str:
            parts.append(f", {param_str}")
        parts.append(")")
        return "".join(parts)

    def __hash__(self) -> int:
        return hash(self._node_id)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Factor):
            return NotImplemented
        return self._node_id == other._node_id


# ---------------------------------------------------------------------------
# SignalExpr — 信号表达式（因子组合器）
# ---------------------------------------------------------------------------

class SignalExpr(DAGNode):
    """信号表达式基类 — 将因子/信号组合为可触发交易的信号.

    信号表达式的求值结果:
    - > 0 : 看多信号 (如金叉)
    - < 0 : 看空信号 (如死叉)
    - = 0 : 无信号
    """

    def __init__(self, op: str, *operands: DAGNode, **kwargs: Any) -> None:
        super().__init__()
        self.op = op
        self.operands: tuple[DAGNode, ...] = operands
        self.kwargs: dict[str, Any] = dict(kwargs)

    def dependencies(self) -> list[DAGNode]:
        return list(self.operands)

    def __repr__(self) -> str:
        kwarg_str = ""
        if self.kwargs:
            kwarg_str = ", " + ", ".join(f"{k}={v}" for k, v in self.kwargs.items())
        operands_str = ", ".join(repr(o) for o in self.operands)
        return f"SignalExpr({self.op!r}, {operands_str}{kwarg_str})"

    def __hash__(self) -> int:
        return hash(self._node_id)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, SignalExpr):
            return NotImplemented
        return self._node_id == other._node_id


# ---------------------------------------------------------------------------
# 信号组合器函数
# ---------------------------------------------------------------------------

def cross_above(signal_a: DAGNode, signal_b: DAGNode) -> SignalExpr:
    """金叉信号: signal_a 从下方上穿 signal_b.

    当 signal_a[t-1] <= signal_b[t-1] 且 signal_a[t] > signal_b[t] 时
    触发看多信号 (返回值 > 0)。

    Args:
        signal_a: 快线因子/信号
        signal_b: 慢线因子/信号

    Returns:
        SignalExpr 节点，编译器据此生成 DAG 边
    """
    return SignalExpr("cross_above", signal_a, signal_b)


def cross_below(signal_a: DAGNode, signal_b: DAGNode) -> SignalExpr:
    """死叉信号: signal_a 从上方下穿 signal_b.

    当 signal_a[t-1] >= signal_b[t-1] 且 signal_a[t] < signal_b[t] 时
    触发看空信号 (返回值 < 0)。

    Args:
        signal_a: 快线因子/信号
        signal_b: 慢线因子/信号

    Returns:
        SignalExpr 节点
    """
    return SignalExpr("cross_below", signal_a, signal_b)


def above(signal_a: DAGNode, signal_b: DAGNode) -> SignalExpr:
    """持续在上方: signal_a > signal_b 时返回正值."""
    return SignalExpr("above", signal_a, signal_b)


def below(signal_a: DAGNode, signal_b: DAGNode) -> SignalExpr:
    """持续在下方: signal_a < signal_b 时返回正值."""
    return SignalExpr("below", signal_a, signal_b)


def and_signal(*signals: DAGNode) -> SignalExpr:
    """信号与: 所有子信号同向时才触发."""
    return SignalExpr("and", *signals)


def or_signal(*signals: DAGNode) -> SignalExpr:
    """信号或: 任一子信号触发即触发."""
    return SignalExpr("or", *signals)


def not_signal(signal: DAGNode) -> SignalExpr:
    """信号取反: 翻转信号方向."""
    return SignalExpr("not", signal)


def threshold(signal: DAGNode, value: float) -> SignalExpr:
    """阈值过滤: 信号绝对值超过 value 时才通过."""
    return SignalExpr("threshold", signal, threshold=value)


# ---------------------------------------------------------------------------
# SignalContext — on_signal 回调的上下文
# ---------------------------------------------------------------------------

@dataclass
class SignalContext:
    """on_signal 回调的运行时上下文.

    提供策略在信号触发时所需的交易接口和状态信息。
    具体实现由回测引擎或实盘引擎注入。
    """

    symbol: str = ""
    price: float = 0.0
    cash: float = 0.0
    position: float = 0.0
    timestamp: object = None
    _orders: list[dict] = field(default_factory=list)

    def order(
        self,
        symbol: str = "",
        side: str = "BUY",
        quantity: float = 0,
        price: float = 0.0,
        order_type: str = "MARKET",
    ) -> None:
        """下单接口.

        Args:
            symbol: 标的代码
            side: 买卖方向 "BUY" / "SELL"
            quantity: 数量
            price: 限价单价格 (MARKET 单忽略)
            order_type: 订单类型 "MARKET" / "LIMIT" / "STOP"
        """
        self._orders.append({
            "symbol": symbol or self.symbol,
            "side": side,
            "quantity": quantity,
            "price": price or self.price,
            "order_type": order_type,
            "timestamp": self.timestamp,
        })

    @property
    def orders(self) -> list[dict]:
        """获取本次 on_signal 产生的所有订单."""
        return list(self._orders)


# ---------------------------------------------------------------------------
# Strategy — DSL 策略基类
# ---------------------------------------------------------------------------

class Strategy(ABC):
    """声明式策略基类.

    子类通过类属性声明 Factor 和 SignalExpr，运行时由编译器
    构建 FactorDAG 并驱动计算。当信号触发时调用 on_signal()。

    也支持传统 on_bar 模式（向后兼容），此时子类实现 on_bar() 即可。

    类属性约定:
    - Factor 实例: 声明因子计算节点
    - SignalExpr 实例: 声明信号组合逻辑
    - 其他属性: 策略参数

    示例::

        @strategy("ma_cross")
        class MACross(Strategy):
            fast_ma = Factor("SMA", period=5, input="close")
            slow_ma = Factor("SMA", period=20, input="close")
            signal = cross_above(fast_ma, slow_ma)

            def on_signal(self, ctx):
                ...
    """

    def __init__(self) -> None:
        # 收集类属性中声明的 Factor 和 SignalExpr
        self._factors: dict[str, Factor] = {}
        self._signals: dict[str, SignalExpr] = {}
        self._signal_values: dict[str, float] = {}
        self._factor_values: dict[str, float] = {}
        self._collect_declarations()
        # 在实例上创建同名属性，遮蔽类属性，初始值为 0.0
        # 这样 self.fast_ma / self.signal 运行时返回 float 值，
        # 而类属性 Factor/SignalExpr 声明仍可通过 _factors/_signals 访问
        for name in self._factors:
            object.__setattr__(self, name, 0.0)
        for name in self._signals:
            object.__setattr__(self, name, 0.0)

    def _collect_declarations(self) -> None:
        """收集类属性中的 Factor 和 SignalExpr 声明."""
        for cls in type(self).__mro__:
            for attr_name, attr_val in vars(cls).items():
                if isinstance(attr_val, Factor) and attr_name not in self._factors:
                    self._factors[attr_name] = attr_val
                elif isinstance(attr_val, SignalExpr) and attr_name not in self._signals:
                    self._signals[attr_name] = attr_val

    @property
    def factors(self) -> dict[str, Factor]:
        """获取所有声明的因子."""
        return dict(self._factors)

    @property
    def signals(self) -> dict[str, SignalExpr]:
        """获取所有声明的信号表达式."""
        return dict(self._signals)

    def get_dag_nodes(self) -> list[DAGNode]:
        """获取所有信号表达式的拓扑排序节点列表（去重）.

        编译器可调用此方法获取完整的 DAG 节点列表，
        按拓扑序排列，保证依赖先于消费者。
        """
        seen_ids: set[str] = set()
        result: list[DAGNode] = []
        for signal_expr in self._signals.values():
            for node in signal_expr.walk():
                if node.node_id not in seen_ids:
                    seen_ids.add(node.node_id)
                    result.append(node)
        return result

    def set_signal_value(self, name: str, value: float) -> None:
        """设置信号值（由引擎在每根 bar 计算后调用）.

        同时更新实例属性，使 self.<signal_name> 返回最新值。
        """
        self._signal_values[name] = value
        if name in self._signals:
            object.__setattr__(self, name, value)

    def set_factor_value(self, name: str, value: float) -> None:
        """设置因子值（由引擎在每根 bar 计算后调用）.

        同时更新实例属性，使 self.<factor_name> 返回最新值。
        """
        self._factor_values[name] = value
        if name in self._factors:
            object.__setattr__(self, name, value)

    def __setattr__(self, name: str, value: Any) -> None:
        """拦截属性设置，保护 Factor/SignalExpr 声明不被意外覆盖.

        允许设置以 _ 开头的内部属性和普通属性，
        但如果属性名对应已声明的 Factor/SignalExpr，则只接受数值类型。
        """
        # 内部属性直接设置
        if name.startswith("_"):
            object.__setattr__(self, name, value)
            return
        # 如果还未初始化（_factors 不存在），直接设置
        if not hasattr(self, "_factors"):
            object.__setattr__(self, name, value)
            return
        # Factor/SignalExpr 声明属性只接受数值
        if name in self._factors or name in self._signals:
            if isinstance(value, (int, float)):
                object.__setattr__(self, name, float(value))
                if name in self._signals:
                    self._signal_values[name] = float(value)
                elif name in self._factors:
                    self._factor_values[name] = float(value)
            else:
                raise TypeError(
                    f"Cannot set '{name}' to {type(value).__name__}: "
                    f"it is a declared Factor/Signal, only numeric values allowed"
                )
        else:
            object.__setattr__(self, name, value)

    # ---- 子类可选实现 ----

    def on_signal(self, ctx: SignalContext) -> None:
        """信号触发回调（声明式策略实现此方法）.

        Args:
            ctx: 信号上下文，提供 symbol, price, cash, position, order()
        """
        pass

    def on_bar(self, bar_data: dict, positions: dict) -> list:
        """传统 on_bar 回调（向后兼容）.

        Args:
            bar_data: {symbol: DataFrame} K线数据
            positions: 当前持仓

        Returns:
            SignalEvent 列表
        """
        return []

    def on_init(self) -> None:
        """策略初始化（可选）."""
        pass

    def on_finish(self) -> None:
        """策略结束（可选）."""
        pass


# ---------------------------------------------------------------------------
# @strategy 装饰器
# ---------------------------------------------------------------------------

def strategy(name: str):
    """策略注册装饰器 — 将 DSL 策略类注册到全局 StrategyRegistry.

    用法::

        @strategy("ma_cross")
        class MACross(Strategy):
            fast_ma = Factor("SMA", period=5, input="close")
            slow_ma = Factor("SMA", period=20, input="close")
            signal = cross_above(fast_ma, slow_ma)

            def on_signal(self, ctx):
                ...

    Args:
        name: 策略注册名，需全局唯一

    Returns:
        装饰器函数
    """
    def decorator(cls: type[Strategy]) -> type[Strategy]:
        # 标记为 DSL 策略，方便引擎区分 on_bar vs on_signal
        cls._dsl_strategy = True  # type: ignore[attr-defined]

        # 收集类级别的 Factor/SignalExpr 声明元信息
        factor_decls: dict[str, Factor] = {}
        signal_decls: dict[str, SignalExpr] = {}
        for base in cls.__mro__:
            for attr_name, attr_val in vars(base).items():
                if isinstance(attr_val, Factor) and attr_name not in factor_decls:
                    factor_decls[attr_name] = attr_val
                elif isinstance(attr_val, SignalExpr) and attr_name not in signal_decls:
                    signal_decls[attr_name] = attr_val

        cls._factor_decls = factor_decls  # type: ignore[attr-defined]
        cls._signal_decls = signal_decls  # type: ignore[attr-defined]

        # 注册到全局注册表
        StrategyRegistry.register(name)(cls)

        return cls

    return decorator


# ---------------------------------------------------------------------------
# 工具函数
# ---------------------------------------------------------------------------

def is_dsl_strategy(cls: type) -> bool:
    """判断一个策略类是否为 DSL 策略."""
    return getattr(cls, "_dsl_strategy", False)


def get_factor_decls(cls: type) -> dict[str, Factor]:
    """获取策略类的因子声明（类级别）."""
    return getattr(cls, "_factor_decls", {})


def get_signal_decls(cls: type) -> dict[str, SignalExpr]:
    """获取策略类的信号声明（类级别）."""
    return getattr(cls, "_signal_decls", {})
