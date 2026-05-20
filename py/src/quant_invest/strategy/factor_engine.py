#!/usr/bin/env python3
"""因子引擎桥接 — 封装 C++ FactorDAG 计算引擎，为回测提供每根 K 线的因子计算.

核心职责:
1. 接收 K 线数据，调用 C++ BuiltInFactors 进行因子计算
2. 在 Python 中计算信号组合器（cross_above, cross_below 等）
3. 将因子值和信号值注入策略实例
4. 支持 C++ 不可用时自动回退到纯 Python

架构::

    ┌─────────────────────────────────────────────────────┐
    │  回测引擎 (BacktestEngine)                           │
    │  每根 K 线调用 FactorEngineBridge.on_bar()           │
    └──────────────────────┬──────────────────────────────┘
                           │
                           ▼
    ┌─────────────────────────────────────────────────────┐
    │  FactorEngineBridge                                  │
    │  ┌─────────────────────────────────────────────┐    │
    │  │ 1. 收集历史 K 线数据                         │    │
    │  │ 2. C++ BuiltInFactors.ma/ema/rsi → 因子值   │    │
    │  │ 3. Python 信号组合器 → 信号值               │    │
    │  │ 4. strategy.set_factor_value() /             │    │
    │  │    strategy.set_signal_value()               │    │
    │  └─────────────────────────────────────────────┘    │
    └─────────────────────────────────────────────────────┘

用法::

    from quant_invest.strategy.factor_engine import FactorEngineBridge

    bridge = FactorEngineBridge(strategy_instance)
    bridge.initialize()

    # 每根 K 线调用
    values = bridge.on_bar(symbol, bar_data)
    # values = {"fast_ma": 25.3, "slow_ma": 20.1, "signal": 1.0}
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from .dsl import Factor, SignalExpr, Strategy

logger = logging.getLogger("quant_invest.strategy.factor_engine")


# ---------------------------------------------------------------------------
# 因子计算结果
# ---------------------------------------------------------------------------

@dataclass
class FactorComputeResult:
    """因子计算结果."""

    factor_values: dict[str, float] = field(default_factory=dict)
    signal_values: dict[str, float] = field(default_factory=dict)
    compute_time_ns: int = 0
    cpp_used: bool = False


# ---------------------------------------------------------------------------
# C++ BuiltInFactors 桥接
# ---------------------------------------------------------------------------

_CPP_AVAILABLE = False

def _check_cpp_available() -> bool:
    """检查 C++ BuiltInFactors 是否可用."""
    try:
        from quant_invest._quant_core import BuiltInFactors
        return True
    except ImportError:
        return False

_CPP_AVAILABLE = _check_cpp_available()


def _compute_factor_cpp(
    kind: str,
    data: np.ndarray,
    **params: Any,
) -> np.ndarray:
    """使用 C++ BuiltInFactors 计算单个因子.

    Args:
        kind: 因子类型（MA/SMA/EMA/RSI/MACD/BOLL/BBANDS）
        data: 输入数据（numpy float64 数组）
        **params: 因子参数

    Returns:
        计算结果（numpy 数组）
    """
    from quant_invest._quant_core import BuiltInFactors

    # 确保输入是 float64 numpy 数组
    if not isinstance(data, np.ndarray):
        data = np.array(data, dtype=np.float64)
    elif data.dtype != np.float64:
        data = data.astype(np.float64)

    if kind in ("SMA", "MA"):
        period = params.get("period", 20)
        return BuiltInFactors.ma(data, period)
    elif kind == "EMA":
        period = params.get("period", 20)
        return BuiltInFactors.ema(data, period)
    elif kind == "RSI":
        period = params.get("period", 14)
        return BuiltInFactors.rsi(data, period)
    else:
        raise ValueError(f"C++ BuiltInFactors 不支持因子类型: {kind}")


# ---------------------------------------------------------------------------
# 纯 Python 因子计算（回退方案）
# ---------------------------------------------------------------------------

def _sma(data: list[float] | np.ndarray, period: int) -> list[float]:
    """简单移动平均（纯 Python）."""
    if isinstance(data, np.ndarray):
        data = data.tolist()
    result: list[float] = []
    for i in range(len(data)):
        if i < period - 1:
            result.append(float("nan"))
        else:
            window = data[i - period + 1 : i + 1]
            result.append(sum(window) / period)
    return result


def _ema(data: list[float] | np.ndarray, period: int) -> list[float]:
    """指数移动平均（纯 Python）."""
    if isinstance(data, np.ndarray):
        data = data.tolist()
    if not data:
        return []
    multiplier = 2.0 / (period + 1)
    result: list[float] = [data[0]]
    for i in range(1, len(data)):
        result.append(data[i] * multiplier + result[-1] * (1 - multiplier))
    return result


def _rsi(data: list[float] | np.ndarray, period: int) -> list[float]:
    """相对强弱指标（纯 Python）."""
    if isinstance(data, np.ndarray):
        data = data.tolist()
    result: list[float] = []
    gains: list[float] = []
    losses: list[float] = []

    for i in range(len(data)):
        if i == 0:
            result.append(float("nan"))
            continue

        change = data[i] - data[i - 1]
        gains.append(max(change, 0.0))
        losses.append(max(-change, 0.0))

        if i < period:
            result.append(float("nan"))
        elif i == period:
            avg_gain = sum(gains[:period]) / period
            avg_loss = sum(losses[:period]) / period
            if avg_loss == 0:
                result.append(100.0)
            else:
                rs = avg_gain / avg_loss
                result.append(100.0 - 100.0 / (1.0 + rs))
        else:
            # EMA-style smoothing
            avg_gain = (gains[-1] + (period - 1) * gains[-2]) / period if len(gains) >= 2 else gains[-1] / period
            avg_loss = (losses[-1] + (period - 1) * losses[-2]) / period if len(losses) >= 2 else losses[-1] / period
            if avg_loss == 0:
                result.append(100.0)
            else:
                rs = avg_gain / avg_loss
                result.append(100.0 - 100.0 / (1.0 + rs))

    return result


def _compute_factor_python(
    kind: str,
    data: list[float] | np.ndarray,
    **params: Any,
) -> list[float]:
    """纯 Python 回退: 计算单个因子.

    Args:
        kind: 因子类型
        data: 输入数据
        **params: 因子参数

    Returns:
        计算结果列表
    """
    if kind in ("SMA", "MA"):
        period = params.get("period", 20)
        return _sma(data, period)
    elif kind == "EMA":
        period = params.get("period", 20)
        return _ema(data, period)
    elif kind == "RSI":
        period = params.get("period", 14)
        return _rsi(data, period)
    else:
        logger.warning("未知因子类型 '%s'，返回原始数据", kind)
        if isinstance(data, np.ndarray):
            return data.tolist()
        return list(data)


# ---------------------------------------------------------------------------
# 信号组合器计算
# ---------------------------------------------------------------------------

def _compute_cross_above(a: list[float], b: list[float]) -> list[float]:
    """金叉信号: a 从下方上穿 b → 1.0, 否则 0.0."""
    result: list[float] = [0.0]
    for i in range(1, len(a)):
        if a[i - 1] <= b[i - 1] and a[i] > b[i]:
            result.append(1.0)
        else:
            result.append(0.0)
    return result


def _compute_cross_below(a: list[float], b: list[float]) -> list[float]:
    """死叉信号: a 从上方下穿 b → -1.0, 否则 0.0."""
    result: list[float] = [0.0]
    for i in range(1, len(a)):
        if a[i - 1] >= b[i - 1] and a[i] < b[i]:
            result.append(-1.0)
        else:
            result.append(0.0)
    return result


def _compute_above(a: list[float], b: list[float]) -> list[float]:
    """a > b 时返回差值."""
    return [a[i] - b[i] if a[i] > b[i] else 0.0 for i in range(len(a))]


def _compute_below(a: list[float], b: list[float]) -> list[float]:
    """a < b 时返回差值绝对值."""
    return [b[i] - a[i] if a[i] < b[i] else 0.0 for i in range(len(a))]


def _compute_and(signals: list[list[float]]) -> list[float]:
    """信号与: 所有子信号同向时取最小绝对值."""
    if not signals:
        return []
    length = len(signals[0])
    result: list[float] = []
    for i in range(length):
        values = [s[i] for s in signals]
        if all(v > 0 for v in values):
            result.append(min(values))
        elif all(v < 0 for v in values):
            result.append(max(values))
        else:
            result.append(0.0)
    return result


def _compute_or(signals: list[list[float]]) -> list[float]:
    """信号或: 任一子信号非零即触发."""
    if not signals:
        return []
    length = len(signals[0])
    result: list[float] = []
    for i in range(length):
        values = [s[i] for s in signals]
        positive = [v for v in values if v > 0]
        negative = [v for v in values if v < 0]
        if positive:
            result.append(max(positive))
        elif negative:
            result.append(min(negative))
        else:
            result.append(0.0)
    return result


def _compute_not(signal: list[float]) -> list[float]:
    """信号取反."""
    return [-v for v in signal]


def _compute_threshold(signal: list[float], value: float) -> list[float]:
    """阈值过滤."""
    return [v if abs(v) >= value else 0.0 for v in signal]


# ---------------------------------------------------------------------------
# FactorEngineBridge — 核心桥接器
# ---------------------------------------------------------------------------

class FactorEngineBridge:
    """因子引擎桥接 — 封装 C++ BuiltInFactors，为回测提供因子计算.

    每根 K 线触发一次因子计算:
    1. 收集历史 K 线数据
    2. 使用 C++ BuiltInFactors 或纯 Python 计算因子
    3. 使用 Python 信号组合器计算信号
    4. 将最新值注入策略实例

    用法::

        bridge = FactorEngineBridge(strategy_instance)
        bridge.initialize()

        # 每根 K 线调用
        values = bridge.on_bar("000001.SZ", {"close": 10.5, "open": 10.3, ...})
    """

    def __init__(
        self,
        strategy: Strategy,
        use_cpp: bool = True,
        history_bars: int = 100,
    ) -> None:
        """初始化因子引擎桥接.

        Args:
            strategy: DSL Strategy 实例
            use_cpp: 是否尝试使用 C++ 因子引擎（默认 True）
            history_bars: 历史数据窗口大小（默认 100 根 K 线）
        """
        self._strategy = strategy
        self._use_cpp = use_cpp and _CPP_AVAILABLE
        self._history_bars = history_bars

        # 因子和信号声明
        self._factors: dict[str, Factor] = strategy.factors
        self._signals: dict[str, SignalExpr] = strategy.signals

        # 自动发现信号操作数中的隐式因子（未声明为类属性的 Factor）
        self._implicit_factors: dict[str, Factor] = {}
        self._node_id_to_attr: dict[str, str] = {}  # node_id → attr_name 映射
        self._discover_implicit_factors()

        # 历史数据缓存: {symbol: {column: [values]}}
        self._history: dict[str, dict[str, list[float]]] = {}

        # 因子计算结果缓存: {attr_name: [values]}
        self._factor_cache: dict[str, list[float]] = {}

        # 信号计算结果缓存: {attr_name: [values]}
        self._signal_cache: dict[str, list[float]] = {}

        # C++ 可用性
        self._cpp_available = self._use_cpp

        # DAG 拓扑序（用于确定计算顺序）
        self._factor_order: list[str] = []
        self._signal_order: list[str] = []

    @property
    def cpp_available(self) -> bool:
        """C++ 因子引擎是否可用."""
        return self._cpp_available

    @property
    def strategy(self) -> Strategy:
        """关联的策略实例."""
        return self._strategy

    def initialize(self) -> None:
        """初始化因子引擎.

        构建因子计算拓扑序，验证因子声明。
        """
        # 确定因子计算顺序（显式因子 + 隐式因子，按声明顺序）
        self._factor_order = list(self._factors.keys()) + list(self._implicit_factors.keys())
        self._signal_order = list(self._signals.keys())

        logger.info(
            "因子引擎初始化: %d 个因子, %d 个信号, C++=%s",
            len(self._factor_order),
            len(self._signal_order),
            self._cpp_available,
        )

        # 尝试使用 C++ FactorRegistry + FactorDAG 进行 DAG 验证
        if self._cpp_available:
            self._validate_dag_cpp()

    def _discover_implicit_factors(self) -> None:
        """自动发现信号操作数中的隐式因子.

        当 DSL 策略在信号组合器中直接使用未声明为类属性的 Factor 时，
        该 Factor 不会出现在 strategy.factors 中。此方法遍历所有信号的
        操作数，发现隐式因子并添加到 _implicit_factors 中。
        """
        # 构建 node_id → attr_name 映射（显式因子）
        for attr_name, factor in self._factors.items():
            self._node_id_to_attr[factor.node_id] = attr_name

        # 遍历信号操作数，发现隐式因子
        for signal in self._signals.values():
            for operand in signal.operands:
                if isinstance(operand, Factor) and operand.node_id not in self._node_id_to_attr:
                    # 隐式因子: 生成一个内部名称
                    implicit_name = f"_implicit_{operand.kind}_{operand.node_id[:8]}"
                    self._implicit_factors[implicit_name] = operand
                    self._node_id_to_attr[operand.node_id] = implicit_name

        if self._implicit_factors:
            logger.info(
                "发现 %d 个隐式因子: %s",
                len(self._implicit_factors),
                list(self._implicit_factors.keys()),
            )

    def _validate_dag_cpp(self) -> None:
        """使用 C++ FactorDAG 验证因子依赖关系."""
        try:
            from quant_invest._quant_core import FactorRegistry, FactorDAG, FactorMeta

            registry = FactorRegistry()

            # 注册因子元数据（仅用于 DAG 验证，不需要计算函数）
            for attr_name, factor in self._factors.items():
                meta = FactorMeta()
                meta.name = attr_name
                meta.inputs = [factor.input]
                meta.outputs = [attr_name]
                meta.version = 1
                # 注册一个空计算函数
                registry.register_factor(meta, lambda inputs: {})

            # 注册信号元数据
            factor_map = {f.node_id: name for name, f in self._factors.items()}
            for attr_name, signal in self._signals.items():
                meta = FactorMeta()
                meta.name = attr_name
                # 信号的输入是操作数
                input_names = []
                for operand in signal.operands:
                    operand_name = factor_map.get(operand.node_id, operand.node_id)
                    input_names.append(operand_name)
                meta.inputs = input_names
                meta.outputs = [attr_name]
                meta.version = 1
                registry.register_factor(meta, lambda inputs: {})

            # 构建 DAG 并验证
            dag = FactorDAG(registry)
            dag.build()
            validation = dag.validate()

            if not validation.valid:
                logger.warning("DAG 验证失败: %s", validation.message)
            else:
                # 使用 C++ 拓扑序
                topo_ids = dag.topological_sort()
                logger.info("DAG 验证通过, 拓扑序: %s", topo_ids)

        except Exception as e:
            logger.debug("C++ DAG 验证跳过: %s", e)

    def on_bar(
        self,
        symbol: str,
        bar_data: dict[str, float],
    ) -> FactorComputeResult:
        """处理一根 K 线数据，计算所有因子和信号.

        Args:
            symbol: 标的代码
            bar_data: K 线数据，如 {"close": 10.5, "open": 10.3, ...}

        Returns:
            FactorComputeResult 包含最新因子值和信号值
        """
        import time
        start_ns = time.perf_counter_ns()

        # 1. 更新历史数据
        self._update_history(symbol, bar_data)

        # 2. 获取历史数据
        history = self._history.get(symbol, {})

        # 3. 计算因子（显式 + 隐式）
        factor_values: dict[str, float] = {}
        all_factors = {**self._factors, **self._implicit_factors}
        for attr_name in self._factor_order:
            factor = all_factors.get(attr_name)
            if factor is None:
                continue
            values = self._compute_factor(factor, history)
            self._factor_cache[attr_name] = values
            if values:
                latest = values[-1]
                if not np.isnan(latest):
                    factor_values[attr_name] = latest
                    # 仅对显式因子注入策略实例
                    if attr_name in self._factors:
                        self._strategy.set_factor_value(attr_name, latest)

        # 4. 计算信号
        signal_values: dict[str, float] = {}
        for attr_name in self._signal_order:
            signal = self._signals[attr_name]
            values = self._compute_signal(signal)
            self._signal_cache[attr_name] = values
            if values:
                latest = values[-1]
                if not np.isnan(latest):
                    signal_values[attr_name] = latest
                    self._strategy.set_signal_value(attr_name, latest)

        elapsed = time.perf_counter_ns() - start_ns

        return FactorComputeResult(
            factor_values=factor_values,
            signal_values=signal_values,
            compute_time_ns=elapsed,
            cpp_used=self._cpp_available,
        )

    def get_factor_series(self, attr_name: str) -> list[float]:
        """获取因子的完整计算序列.

        Args:
            attr_name: 因子属性名

        Returns:
            因子值列表
        """
        return self._factor_cache.get(attr_name, [])

    def get_signal_series(self, attr_name: str) -> list[float]:
        """获取信号的完整计算序列.

        Args:
            attr_name: 信号属性名

        Returns:
            信号值列表
        """
        return self._signal_cache.get(attr_name, [])

    def reset(self) -> None:
        """重置引擎状态（新回测开始时调用）."""
        self._history.clear()
        self._factor_cache.clear()
        self._signal_cache.clear()

    # ------------------------------------------------------------------
    # 内部方法
    # ------------------------------------------------------------------

    def _update_history(self, symbol: str, bar_data: dict[str, float]) -> None:
        """更新历史数据缓存.

        维护一个滑动窗口，最多保留 history_bars 根 K 线。
        """
        if symbol not in self._history:
            self._history[symbol] = {}

        for col, value in bar_data.items():
            if col not in self._history[symbol]:
                self._history[symbol][col] = []
            self._history[symbol][col].append(float(value))

            # 滑动窗口: 超过 history_bars 时截断
            if len(self._history[symbol][col]) > self._history_bars:
                self._history[symbol][col] = self._history[symbol][col][-self._history_bars:]

    def _compute_factor(
        self,
        factor: Factor,
        history: dict[str, list[float]],
    ) -> list[float]:
        """计算单个因子.

        优先使用 C++ BuiltInFactors，不可用时回退到纯 Python。

        Args:
            factor: DSL Factor 声明
            history: 历史数据

        Returns:
            因子值列表
        """
        input_data = history.get(factor.input, [])
        if not input_data:
            return []

        if self._cpp_available:
            try:
                result = _compute_factor_cpp(
                    factor.kind,
                    np.array(input_data, dtype=np.float64),
                    **factor.params,
                )
                return result.tolist()
            except Exception as e:
                logger.debug(
                    "C++ 因子计算失败 (%s)，回退到 Python: %s",
                    factor.kind, e,
                )

        # 纯 Python 回退
        return _compute_factor_python(factor.kind, input_data, **factor.params)

    def _compute_signal(self, signal: SignalExpr) -> list[float]:
        """计算单个信号.

        信号组合器始终在 Python 中计算（C++ 不提供信号组合器 API）。

        Args:
            signal: DSL SignalExpr 声明

        Returns:
            信号值列表
        """
        op = signal.op
        operands = signal.operands

        # 获取操作数数据（通过 node_id 匹配因子/信号缓存）
        operand_data: list[list[float]] = []
        for operand in operands:
            data = self._find_operand_data(operand)
            if not data:
                return []
            operand_data.append(data)

        # 检查操作数长度一致性
        lengths = [len(d) for d in operand_data]
        if len(set(lengths)) > 1:
            # 对齐到最短长度
            min_len = min(lengths)
            operand_data = [d[:min_len] for d in operand_data]

        # 计算信号
        if op == "cross_above":
            return _compute_cross_above(operand_data[0], operand_data[1])
        elif op == "cross_below":
            return _compute_cross_below(operand_data[0], operand_data[1])
        elif op == "above":
            return _compute_above(operand_data[0], operand_data[1])
        elif op == "below":
            return _compute_below(operand_data[0], operand_data[1])
        elif op == "and":
            return _compute_and(operand_data)
        elif op == "or":
            return _compute_or(operand_data)
        elif op == "not":
            return _compute_not(operand_data[0])
        elif op == "threshold":
            thresh = signal.kwargs.get("threshold", 0.0)
            return _compute_threshold(operand_data[0], thresh)
        else:
            logger.warning("未知信号操作 '%s'", op)
            return [0.0] * len(operand_data[0]) if operand_data else []

    def _find_operand_data(self, operand: Any) -> list[float]:
        """通过 node_id 查找操作数的计算结果.

        使用 _node_id_to_attr 映射将 node_id 转换为属性名，
        然后在因子/信号缓存中查找。

        Args:
            operand: DAGNode 操作数

        Returns:
            计算结果列表
        """
        node_id = operand.node_id

        # 通过映射查找属性名
        attr_name = self._node_id_to_attr.get(node_id)
        if attr_name and attr_name in self._factor_cache:
            return self._factor_cache[attr_name]
        if attr_name and attr_name in self._signal_cache:
            return self._signal_cache[attr_name]

        # 兜底: 遍历所有缓存
        for name, factor in self._factors.items():
            if factor.node_id == node_id:
                return self._factor_cache.get(name, [])

        for name, factor in self._implicit_factors.items():
            if factor.node_id == node_id:
                return self._factor_cache.get(name, [])

        for name, signal in self._signals.items():
            if signal.node_id == node_id:
                return self._signal_cache.get(name, [])

        logger.warning("未找到操作数数据: node_id=%s", node_id)
        return []
