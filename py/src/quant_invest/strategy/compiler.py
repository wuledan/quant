#!/usr/bin/env python3
"""策略编译器 — 将 DSL 策略声明编译为 C++ FactorDAG 执行图.

编译流程:
1. 从 DSL Strategy 类提取 Factor 和 SignalExpr 声明
2. 为每个 Factor 在 C++ FactorRegistry 中注册计算函数
3. 为每个 SignalExpr 生成组合计算函数并注册
4. 调用 FactorDAG.build() 构建依赖图
5. 创建 FactorComputer 用于执行

用法::

    from quant_invest.strategy.compiler import StrategyCompiler

    compiler = StrategyCompiler()
    compiled = compiler.compile(my_dsl_strategy_instance)
    result = compiled.compute(input_data={"close": [...]})
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any, Protocol

from .dsl import DAGNode, Factor, SignalExpr, Strategy
from .dsl import cross_above, cross_below, above, below
from .dsl import and_signal, or_signal, not_signal, threshold

logger = logging.getLogger("quant_invest.strategy.compiler")


# ---------------------------------------------------------------------------
# 编译产物
# ---------------------------------------------------------------------------

@dataclass
class CompiledStrategy:
    """编译后的策略 — 包含 C++ 因子引擎和信号评估逻辑.

    Attributes:
        name: 策略名
        strategy: 原始 DSL Strategy 实例
        factor_names: 已注册的因子名列表 (C++ FactorRegistry 中的名字)
        signal_names: 已注册的信号名列表
        computer: C++ FactorComputer 实例 (如果 C++ 模块可用)
        registry: C++ FactorRegistry 实例
        dag: C++ FactorDAG 实例
    """

    name: str
    strategy: Strategy
    factor_names: list[str] = field(default_factory=list)
    signal_names: list[str] = field(default_factory=list)
    computer: Any = None  # C++ FactorComputer
    registry: Any = None  # C++ FactorRegistry
    dag: Any = None       # C++ FactorDAG

    def compute(self, input_data: dict[str, list[float]]) -> dict[str, Any]:
        """执行因子计算.

        Args:
            input_data: 输入数据，如 {"close": [1.0, 2.0, ...]}

        Returns:
            计算结果，包含因子值和信号值
        """
        if self.computer is None:
            raise RuntimeError(
                "C++ FactorComputer not available. "
                "Ensure _quant_core module is compiled and loaded."
            )
        result = self.computer.compute_all(input_data)
        return {
            "success": result.success,
            "outputs": result.outputs,
            "compute_time_ns": result.compute_time_ns,
        }

    def compute_factor(
        self, factor_name: str, input_data: dict[str, list[float]]
    ) -> dict[str, Any]:
        """计算单个因子."""
        if self.computer is None:
            raise RuntimeError("C++ FactorComputer not available.")
        result = self.computer.compute_factor(factor_name, input_data)
        return {
            "success": result.success,
            "outputs": result.outputs,
            "compute_time_ns": result.compute_time_ns,
        }


# ---------------------------------------------------------------------------
# 因子计算函数生成器
# ---------------------------------------------------------------------------

def _make_factor_compute_fn(factor: Factor) -> Any:
    """为 Factor 生成计算函数.

    如果 C++ BuiltInFactors 中有对应的算子，直接使用；
    否则生成一个 Python 计算函数（回退方案）。

    Args:
        factor: DSL Factor 声明

    Returns:
        可被 C++ FactorRegistry.register_factor 接受的计算函数
    """
    kind = factor.kind
    params = factor.params
    input_col = factor.input

    # 内置因子映射表
    _BUILTIN_COMPUTE: dict[str, Any] = {}

    def _compute(inputs: dict[str, list[float]]) -> dict[str, list[float]]:
        """Python 回退计算函数."""
        data = inputs.get(input_col, [])
        if not data:
            return {factor._node_id: []}

        if kind == "SMA" or kind == "MA":
            period = params.get("period", 20)
            result = _sma(data, period)
            return {factor._node_id: result}
        elif kind == "EMA":
            period = params.get("period", 20)
            result = _ema(data, period)
            return {factor._node_id: result}
        elif kind == "RSI":
            period = params.get("period", 14)
            result = _rsi(data, period)
            return {factor._node_id: result}
        else:
            logger.warning("未知因子类型 '%s'，返回原始数据", kind)
            return {factor._node_id: data}

    return _compute


def _make_signal_compute_fn(
    signal: SignalExpr,
    factor_map: dict[str, str],
) -> Any:
    """为 SignalExpr 生成计算函数.

    Args:
        signal: DSL SignalExpr 声明
        factor_map: {node_id -> registered_factor_name} 映射

    Returns:
        计算函数
    """

    def _compute(inputs: dict[str, list[float]]) -> dict[str, list[float]]:
        """信号计算函数."""
        op = signal.op
        operands = signal.operands

        # 获取操作数数据
        operand_data: list[list[float]] = []
        for operand in operands:
            node_key = factor_map.get(operand.node_id, operand.node_id)
            data = inputs.get(node_key, [])
            if not data:
                return {signal.node_id: []}
            operand_data.append(data)

        if op == "cross_above":
            result = _cross_above(operand_data[0], operand_data[1])
        elif op == "cross_below":
            result = _cross_below(operand_data[0], operand_data[1])
        elif op == "above":
            result = _above(operand_data[0], operand_data[1])
        elif op == "below":
            result = _below(operand_data[0], operand_data[1])
        elif op == "and":
            result = _and_signals(operand_data)
        elif op == "or":
            result = _or_signals(operand_data)
        elif op == "not":
            result = _not_signal(operand_data[0])
        elif op == "threshold":
            thresh = signal.kwargs.get("threshold", 0.0)
            result = _threshold(operand_data[0], thresh)
        else:
            logger.warning("未知信号操作 '%s'", op)
            result = [0.0] * len(operand_data[0]) if operand_data else []

        return {signal.node_id: result}

    return _compute


# ---------------------------------------------------------------------------
# 纯 Python 因子计算实现（回退方案，当 C++ 不可用时使用）
# ---------------------------------------------------------------------------

def _sma(data: list[float], period: int) -> list[float]:
    """简单移动平均."""
    result: list[float] = []
    for i in range(len(data)):
        if i < period - 1:
            result.append(float("nan"))
        else:
            window = data[i - period + 1 : i + 1]
            result.append(sum(window) / period)
    return result


def _ema(data: list[float], period: int) -> list[float]:
    """指数移动平均."""
    if not data:
        return []
    multiplier = 2.0 / (period + 1)
    result: list[float] = [data[0]]
    for i in range(1, len(data)):
        result.append(data[i] * multiplier + result[-1] * (1 - multiplier))
    return result


def _rsi(data: list[float], period: int) -> list[float]:
    """相对强弱指标."""
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
            prev_avg_gain = (result[-1] if result else 50.0)
            # Simplified: use EMA-style smoothing
            avg_gain = (gains[-1] + (period - 1) * gains[-2]) / period if len(gains) >= 2 else gains[-1] / period
            avg_loss = (losses[-1] + (period - 1) * losses[-2]) / period if len(losses) >= 2 else losses[-1] / period
            if avg_loss == 0:
                result.append(100.0)
            else:
                rs = avg_gain / avg_loss
                result.append(100.0 - 100.0 / (1.0 + rs))

    return result


def _cross_above(a: list[float], b: list[float]) -> list[float]:
    """金叉信号: a 从下方上穿 b → 1.0, 否则 0.0."""
    result: list[float] = [0.0]  # 第一根 bar 无法判断
    for i in range(1, len(a)):
        if a[i - 1] <= b[i - 1] and a[i] > b[i]:
            result.append(1.0)
        else:
            result.append(0.0)
    return result


def _cross_below(a: list[float], b: list[float]) -> list[float]:
    """死叉信号: a 从上方下穿 b → -1.0, 否则 0.0."""
    result: list[float] = [0.0]
    for i in range(1, len(a)):
        if a[i - 1] >= b[i - 1] and a[i] < b[i]:
            result.append(-1.0)
        else:
            result.append(0.0)
    return result


def _above(a: list[float], b: list[float]) -> list[float]:
    """a > b 时返回正值 (差值)."""
    return [a[i] - b[i] if a[i] > b[i] else 0.0 for i in range(len(a))]


def _below(a: list[float], b: list[float]) -> list[float]:
    """a < b 时返回正值 (差值绝对值)."""
    return [b[i] - a[i] if a[i] < b[i] else 0.0 for i in range(len(a))]


def _and_signals(signals: list[list[float]]) -> list[float]:
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


def _or_signals(signals: list[list[float]]) -> list[float]:
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


def _not_signal(signal: list[float]) -> list[float]:
    """信号取反."""
    return [-v for v in signal]


def _threshold(signal: list[float], value: float) -> list[float]:
    """阈值过滤."""
    return [v if abs(v) >= value else 0.0 for v in signal]


# ---------------------------------------------------------------------------
# StrategyCompiler — 核心编译器
# ---------------------------------------------------------------------------

class StrategyCompiler:
    """策略编译器 — 将 DSL 策略编译为可执行的因子计算图.

    编译步骤:
    1. 提取 Factor 和 SignalExpr 声明
    2. 在 C++ FactorRegistry 中注册因子计算函数
    3. 在 C++ FactorRegistry 中注册信号计算函数
    4. 调用 FactorDAG.build() 构建依赖图
    5. 创建 FactorComputer

    如果 C++ _quant_core 模块不可用，回退到纯 Python 计算。
    """

    def __init__(self, use_cpp: bool = True) -> None:
        """初始化编译器.

        Args:
            use_cpp: 是否尝试使用 C++ 因子引擎（默认 True）
        """
        self._use_cpp = use_cpp
        self._cpp_available = False
        self._FactorRegistry: type | None = None
        self._FactorDAG: type | None = None
        self._FactorComputer: type | None = None
        self._FactorMeta: type | None = None
        self._BuiltInFactors: type | None = None

        if use_cpp:
            self._try_import_cpp()

    def _try_import_cpp(self) -> None:
        """尝试导入 C++ 因子引擎模块."""
        try:
            from quant_invest.c_bindings import (
                BuiltInFactors,
                FactorComputer,
                FactorDAG,
                FactorMeta,
                FactorRegistry,
            )

            self._FactorRegistry = FactorRegistry
            self._FactorDAG = FactorDAG
            self._FactorComputer = FactorComputer
            self._FactorMeta = FactorMeta
            self._BuiltInFactors = BuiltInFactors
            self._cpp_available = True
            logger.info("C++ 因子引擎加载成功")
        except (ImportError, Exception) as e:
            self._cpp_available = False
            logger.info("C++ 因子引擎不可用，将使用纯 Python 回退: %s", e)

    @property
    def cpp_available(self) -> bool:
        """C++ 因子引擎是否可用."""
        return self._cpp_available

    def compile(self, strategy_instance: Strategy, name: str = "") -> CompiledStrategy:
        """编译 DSL 策略.

        Args:
            strategy_instance: DSL Strategy 实例
            name: 策略名（用于日志和调试）

        Returns:
            CompiledStrategy 编译产物
        """
        name = name or type(strategy_instance).__name__

        # 1. 提取声明
        factors = strategy_instance.factors
        signals = strategy_instance.signals
        logger.info(
            "编译策略 '%s': %d 个因子, %d 个信号",
            name,
            len(factors),
            len(signals),
        )

        if self._cpp_available:
            return self._compile_cpp(name, strategy_instance, factors, signals)
        else:
            return self._compile_python(name, strategy_instance, factors, signals)

    def _compile_cpp(
        self,
        name: str,
        strategy_instance: Strategy,
        factors: dict[str, Factor],
        signals: dict[str, SignalExpr],
    ) -> CompiledStrategy:
        """使用 C++ 因子引擎编译."""
        assert self._FactorRegistry is not None
        assert self._FactorDAG is not None
        assert self._FactorComputer is not None
        assert self._FactorMeta is not None

        # 创建 C++ FactorRegistry
        registry = self._FactorRegistry()

        # 注册内置因子
        if self._BuiltInFactors is not None:
            self._BuiltInFactors.register_all(registry)
            logger.debug("已注册内置因子")

        # 构建 node_id → 注册因子名 的映射
        factor_map: dict[str, str] = {}

        # 2. 注册 DSL Factor
        for attr_name, factor in factors.items():
            registered_name = self._register_factor_cpp(
                registry, attr_name, factor
            )
            factor_map[factor.node_id] = registered_name
            logger.debug("注册因子: %s → %s", attr_name, registered_name)

        # 3. 注册 SignalExpr
        for attr_name, signal in signals.items():
            registered_name = self._register_signal_cpp(
                registry, attr_name, signal, factor_map
            )
            factor_map[signal.node_id] = registered_name
            logger.debug("注册信号: %s → %s", attr_name, registered_name)

        # 4. 构建 DAG
        dag = self._FactorDAG(registry)
        dag.build()

        # 验证 DAG
        validation = dag.validate()
        if not validation.valid:
            raise ValueError(
                f"策略 '{name}' DAG 验证失败: {validation.message}"
            )
        logger.info("DAG 验证通过, 拓扑序: %s", dag.topological_sort())

        # 5. 创建 FactorComputer
        computer = self._FactorComputer(
            registry,  # type: ignore[arg-type]
            dag,       # type: ignore[arg-type]
        )

        return CompiledStrategy(
            name=name,
            strategy=strategy_instance,
            factor_names=list(factors.keys()),
            signal_names=list(signals.keys()),
            computer=computer,
            registry=registry,
            dag=dag,
        )

    def _register_factor_cpp(
        self,
        registry: Any,
        attr_name: str,
        factor: Factor,
    ) -> str:
        """在 C++ FactorRegistry 中注册一个 Factor.

        Returns:
            注册后的因子名
        """
        assert self._FactorMeta is not None

        # 因子名: 使用 attr_name 作为注册名，方便后续查找
        registered_name = f"dsl_{attr_name}_{factor.node_id[:6]}"

        # 检查是否已有内置因子可直接使用
        builtin_name = self._map_to_builtin_name(factor.kind)
        if builtin_name and registry.has_factor_by_name(builtin_name):
            # 内置因子已注册，创建一个别名
            logger.debug(
                "因子 '%s' 映射到内置因子 '%s'", factor.kind, builtin_name
            )

        meta = self._FactorMeta()
        meta.name = registered_name
        meta.description = f"DSL factor: {factor.kind}({factor.params})"
        meta.inputs = [factor.input]
        meta.outputs = [registered_name]
        meta.version = 1

        compute_fn = _make_factor_compute_fn(factor)
        registry.register_factor(meta, compute_fn)

        return registered_name

    def _register_signal_cpp(
        self,
        registry: Any,
        attr_name: str,
        signal: SignalExpr,
        factor_map: dict[str, str],
    ) -> str:
        """在 C++ FactorRegistry 中注册一个 SignalExpr.

        Returns:
            注册后的信号名
        """
        assert self._FactorMeta is not None

        registered_name = f"dsl_{attr_name}_{signal.node_id[:6]}"

        # 信号的输入是所有操作数的输出
        input_names = []
        for operand in signal.operands:
            operand_name = factor_map.get(operand.node_id, operand.node_id)
            input_names.append(operand_name)

        meta = self._FactorMeta()
        meta.name = registered_name
        meta.description = f"DSL signal: {signal.op}({signal.kwargs})"
        meta.inputs = input_names
        meta.outputs = [registered_name]
        meta.version = 1

        compute_fn = _make_signal_compute_fn(signal, factor_map)
        registry.register_factor(meta, compute_fn)

        return registered_name

    def _compile_python(
        self,
        name: str,
        strategy_instance: Strategy,
        factors: dict[str, Factor],
        signals: dict[str, SignalExpr],
    ) -> CompiledStrategy:
        """纯 Python 回退编译（当 C++ 不可用时）."""
        logger.info("使用纯 Python 回退编译策略 '%s'", name)

        # 纯 Python 模式下，不创建 C++ 对象
        # 但仍然构建计算图元数据，供纯 Python 执行器使用
        return CompiledStrategy(
            name=name,
            strategy=strategy_instance,
            factor_names=list(factors.keys()),
            signal_names=list(signals.keys()),
            computer=None,  # 纯 Python 模式下无 C++ FactorComputer
            registry=None,
            dag=None,
        )

    @staticmethod
    def _map_to_builtin_name(kind: str) -> str | None:
        """将 DSL Factor kind 映射到 C++ 内置因子名.

        DSL 使用更直观的名字 (SMA, EMA, RSI, MACD, BBANDS)，
        C++ BuiltInFactors 使用 MA, EMA, RSI, MACD, BOLL。
        """
        _KIND_MAP = {
            "SMA": "MA",
            "MA": "MA",
            "EMA": "EMA",
            "RSI": "RSI",
            "MACD": "MACD",
            "BBANDS": "BOLL",
            "BOLL": "BOLL",
        }
        return _KIND_MAP.get(kind)


# ---------------------------------------------------------------------------
# 纯 Python 执行器（当 C++ 不可用时使用）
# ---------------------------------------------------------------------------

class PythonExecutor:
    """纯 Python 因子执行器 — 当 C++ 模块不可用时使用.

    按拓扑序执行因子和信号计算，将结果注入 Strategy 实例。
    """

    def __init__(self, compiled: CompiledStrategy) -> None:
        self._compiled = compiled
        self._strategy = compiled.strategy
        self._factors = compiled.strategy.factors
        self._signals = compiled.strategy.signals

    def execute(self, input_data: dict[str, list[float]]) -> dict[str, list[float]]:
        """执行因子和信号计算.

        Args:
            input_data: 输入数据，如 {"close": [1.0, 2.0, ...]}

        Returns:
            所有因子和信号的计算结果
        """
        results: dict[str, list[float]] = dict(input_data)

        # 按拓扑序计算因子
        for attr_name, factor in self._factors.items():
            compute_fn = _make_factor_compute_fn(factor)
            factor_outputs = compute_fn(results)
            results.update(factor_outputs)
            # 同时用 attr_name 作为 key（方便查找）
            results[attr_name] = results[factor.node_id]

        # 按拓扑序计算信号
        # 先构建 node_id → attr_name 映射
        factor_map: dict[str, str] = {}
        for attr_name, factor in self._factors.items():
            factor_map[factor.node_id] = factor.node_id
        for attr_name, signal in self._signals.items():
            factor_map[signal.node_id] = signal.node_id

        for attr_name, signal in self._signals.items():
            compute_fn = _make_signal_compute_fn(signal, factor_map)
            signal_outputs = compute_fn(results)
            results.update(signal_outputs)
            results[attr_name] = results[signal.node_id]

        return results

    def execute_and_update(
        self, input_data: dict[str, list[float]]
    ) -> dict[str, float]:
        """执行计算并更新 Strategy 实例的信号值.

        Args:
            input_data: 输入数据

        Returns:
            最新一根 bar 的因子/信号值
        """
        all_results = self.execute(input_data)
        latest: dict[str, float] = {}

        for attr_name in self._factors:
            values = all_results.get(attr_name, [])
            if values:
                val = values[-1]
                latest[attr_name] = val
                self._strategy.set_factor_value(attr_name, val)

        for attr_name in self._signals:
            values = all_results.get(attr_name, [])
            if values:
                val = values[-1]
                latest[attr_name] = val
                self._strategy.set_signal_value(attr_name, val)

        return latest
