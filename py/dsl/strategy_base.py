# strategy_base.py — Strategy DSL base class with typed operators
#
# Standalone DSL for defining factor strategies. Subclass StrategyBase,
# override build() to wire up operators (sma, ema, cross_above, etc.),
# and compile to IR JSON via compiler.IRCompiler.
#
# Usage:
#   class MACross(StrategyBase):
#       fast_period = 5
#       slow_period = 20
#
#       def build(self):
#           fast = sma("kline.close", self.fast_period)
#           slow = sma("kline.close", self.slow_period)
#           sig = cross_above(fast, slow)
#           return [fast, slow, sig]

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from typing import Any


# ── IR type constants ──

TS_FLOAT = {"base_type": "TimeSeries", "inner_type": "float"}
TS_INT = {"base_type": "TimeSeries", "inner_type": "int64"}
SIGNAL_BOOL = {"base_type": "Signal", "inner_type": "bool"}
SCALAR_FLOAT = {"base_type": "Scalar", "inner_type": "float"}


# ── Node representations ──

@dataclass
class OpNode:
    """A node in the strategy computation graph."""
    id: str
    op_type: str
    inputs: dict[str, Any] = field(default_factory=dict)
    outputs: dict[str, Any] = field(default_factory=dict)
    params: dict[str, float] = field(default_factory=dict)


def _make_port(name: str, type_spec: dict, source: str) -> dict:
    return {"name": name, "type": type_spec, "source": source}


def _make_data_source(field: str) -> dict:
    """Create an input dict referencing a data field."""
    return {"type": "data", "field": field}


def _make_node_ref(node_id: str, port: str = "value") -> dict:
    """Create an input dict referencing another node's output."""
    return {"type": "node", "node_id": node_id, "port": port}


# ── Operator functions ──

_node_counter: int = 0


def _next_id(prefix: str) -> str:
    global _node_counter
    _node_counter += 1
    return f"{prefix}_{_node_counter}"


def sma(field: str, period: int) -> OpNode:
    """Simple Moving Average operator."""
    node_id = _next_id("sma")
    return OpNode(
        id=node_id,
        op_type="SMA",
        inputs={"price": _make_data_source(field)},
        outputs={
            "value": _make_port("value", TS_FLOAT, f"node.{node_id}.value"),
        },
        params={"period": float(period)},
    )


def ema(field: str, period: int) -> OpNode:
    """Exponential Moving Average operator."""
    node_id = _next_id("ema")
    return OpNode(
        id=node_id,
        op_type="EMA",
        inputs={"price": _make_data_source(field)},
        outputs={
            "value": _make_port("value", TS_FLOAT, f"node.{node_id}.value"),
        },
        params={"period": float(period)},
    )


def cross_above(a: OpNode, b: OpNode) -> OpNode:
    """Golden cross: a crosses above b."""
    node_id = _next_id("cross")
    return OpNode(
        id=node_id,
        op_type="CROSS_ABOVE",
        inputs={
            "fast": _make_node_ref(a.id),
            "slow": _make_node_ref(b.id),
        },
        outputs={
            "signal": _make_port("signal", SIGNAL_BOOL, f"node.{node_id}.signal"),
        },
        params={},
    )


def cross_below(a: OpNode, b: OpNode) -> OpNode:
    """Death cross: a crosses below b."""
    node_id = _next_id("cross")
    return OpNode(
        id=node_id,
        op_type="CROSS_BELOW",
        inputs={
            "fast": _make_node_ref(a.id),
            "slow": _make_node_ref(b.id),
        },
        outputs={
            "signal": _make_port("signal", SIGNAL_BOOL, f"node.{node_id}.signal"),
        },
        params={},
    )


def threshold(node: OpNode, level: float) -> OpNode:
    """Threshold filter on a node output."""
    node_id = _next_id("thresh")
    return OpNode(
        id=node_id,
        op_type="THRESHOLD",
        inputs={"input": _make_node_ref(node.id)},
        outputs={
            "signal": _make_port("signal", SIGNAL_BOOL, f"node.{node_id}.signal"),
        },
        params={"threshold": level},
    )


# ── Strategy base class ──

class StrategyBase:
    """Base class for strategy definitions.

    Subclasses set class-level attributes for parameters and override
    build() to return a list of OpNode operators forming the computation graph.

    Example:
        class MACross(StrategyBase):
            fast_period = 5
            slow_period = 20

            def build(self):
                fast = sma("kline.close", self.fast_period)
                slow = sma("kline.close", self.slow_period)
                sig = cross_above(fast, slow)
                return [fast, slow, sig]
    """

    _strategy_name: str = ""

    def __init_subclass__(cls, **kwargs: Any) -> None:
        super().__init_subclass__(**kwargs)
        if not cls._strategy_name:
            cls._strategy_name = cls.__name__

    def build(self) -> list[OpNode]:
        """Override to define the computation graph.

        Returns:
            List of OpNode instances (operators wired together).
        """
        raise NotImplementedError

    def get_params(self) -> dict[str, Any]:
        """Extract user-defined parameters (non-dunder class attributes)."""
        params: dict[str, Any] = {}
        for key in dir(self):
            if key.startswith("_"):
                continue
            val = getattr(self, key)
            if callable(val):
                continue
            params[key] = val
        return params
