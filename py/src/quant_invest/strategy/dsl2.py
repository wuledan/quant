# dsl2.py — Strategy DSL v2: typed I/O nodes for IR compilation
#
# Key changes from DSL v1:
# - Factor/Signal have explicit typed inputs and outputs
# - PortRef enables connection inference between nodes
# - DataField provides typed data source references
# - Compiles to IR (.graph JSON) instead of directly registering C++ objects

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Optional


# ── Data source fields ──

class DataField:
    """Data source field references — maps to StorageEngine columns."""
    open = "kline.open"
    high = "kline.high"
    low = "kline.low"
    close = "kline.close"
    volume = "kline.volume"
    amount = "kline.amount"
    vwap = "kline.vwap"
    position = "portfolio.position"
    cash = "portfolio.cash"


# ── Type specifications ──

@dataclass(frozen=True)
class TypeSpec:
    """Type specification for node ports."""
    base_type: str    # "TimeSeries", "Signal", "Scalar"
    inner_type: str   # "float", "int64", "bool"

    def compatible(self, other: TypeSpec) -> bool:
        return self.base_type == other.base_type and self.inner_type == other.inner_type

    def to_dict(self) -> dict:
        return {"base_type": self.base_type, "inner_type": self.inner_type}


# Common type aliases
TS_FLOAT = TypeSpec("TimeSeries", "float")
TS_INT = TypeSpec("TimeSeries", "int64")
SIGNAL_FLOAT = TypeSpec("Signal", "float")
SCALAR_FLOAT = TypeSpec("Scalar", "float")
SCALAR_INT = TypeSpec("Scalar", "int64")


# ── Port references ──

@dataclass(frozen=True)
class PortRef:
    """Reference to a node's output port — used for connection inference."""
    node_id: str
    port_name: str
    port_type: TypeSpec

    def to_dict(self) -> dict:
        return {
            "name": self.port_name,
            "type": self.port_type.to_dict(),
            "source": f"node.{self.node_id}.{self.port_name}",
        }


# ── DAG Node base ──

class DAGNode:
    """Base class for strategy computation graph nodes."""
    _node_id: str
    _strategy_cls: Optional[type] = None

    def output(self, name: str = "value") -> PortRef:
        """Get a reference to an output port of this node."""
        outputs = self._get_outputs()
        if name not in outputs:
            raise ValueError(f"Node {self._node_id} has no output port '{name}'. "
                             f"Available: {list(outputs.keys())}")
        return PortRef(self._node_id, name, outputs[name])

    def _get_outputs(self) -> dict[str, TypeSpec]:
        raise NotImplementedError


# ── Factor node ──

class Factor(DAGNode):
    """Factor computation node with typed inputs and outputs.

    Usage:
        fast_ma = Factor(
            op="SMA",
            inputs={"price": DataField.close},
            outputs={"value": TS_FLOAT},
            params={"period": 5},
        )
    """
    _counter = 0

    def __init__(
        self,
        op: str,
        inputs: dict[str, DataField | PortRef],
        outputs: dict[str, TypeSpec] | None = None,
        params: dict[str, Any] | None = None,
    ):
        Factor._counter += 1
        self._node_id = f"_factor_{Factor._counter}"
        self.op = op
        self.inputs = inputs
        self.outputs = outputs or {"value": TS_FLOAT}
        self.params = params or {}

    def __set_name__(self, owner, name):
        """When assigned as class attribute, use the attribute name as node ID."""
        self._node_id = name
        self._strategy_cls = owner

    def _get_outputs(self) -> dict[str, TypeSpec]:
        return self.outputs

    def to_node_def(self) -> dict:
        """Convert to IR NodeDef dict."""
        input_defs = {}
        for port_name, source in self.inputs.items():
            if isinstance(source, PortRef):
                input_defs[port_name] = source.to_dict()
            else:
                # DataField — string reference
                input_defs[port_name] = {
                    "name": port_name,
                    "type": TS_FLOAT.to_dict(),
                    "source": source if isinstance(source, str) else str(source),
                }

        output_defs = {}
        for port_name, type_spec in self.outputs.items():
            output_defs[port_name] = {
                "name": port_name,
                "type": type_spec.to_dict(),
                "source": "",
            }

        return {
            "id": self._node_id,
            "op_type": self.op,
            "inputs": input_defs,
            "outputs": output_defs,
            "params": {k: float(v) for k, v in self.params.items()},
        }


# ── Signal node ──

class Signal(DAGNode):
    """Signal computation node with typed inputs and outputs.

    Usage:
        signal = cross_above(fast=fast_ma.output("value"), slow=slow_ma.output("value"))
    """
    _counter = 0

    def __init__(
        self,
        op: str,
        inputs: dict[str, PortRef],
        outputs: dict[str, TypeSpec] | None = None,
        params: dict[str, Any] | None = None,
    ):
        Signal._counter += 1
        self._node_id = f"_signal_{Signal._counter}"
        self.op = op
        self.inputs = inputs
        self.outputs = outputs or {"value": SIGNAL_FLOAT}
        self.params = params or {}

    def __set_name__(self, owner, name):
        self._node_id = name
        self._strategy_cls = owner

    def _get_outputs(self) -> dict[str, TypeSpec]:
        return self.outputs

    def to_node_def(self) -> dict:
        """Convert to IR NodeDef dict."""
        input_defs = {}
        for port_name, port_ref in self.inputs.items():
            input_defs[port_name] = port_ref.to_dict()

        output_defs = {}
        for port_name, type_spec in self.outputs.items():
            output_defs[port_name] = {
                "name": port_name,
                "type": type_spec.to_dict(),
                "source": "",
            }

        return {
            "id": self._node_id,
            "op_type": self.op,
            "inputs": input_defs,
            "outputs": output_defs,
            "params": {k: float(v) for k, v in self.params.items()},
        }


# ── Signal combinators ──

def cross_above(**operands: PortRef) -> Signal:
    """Golden cross signal — fires when first operand crosses above second."""
    if len(operands) != 2:
        raise ValueError("cross_above requires exactly 2 named operands (e.g., fast=..., slow=...)")
    return Signal(op="CROSS_ABOVE", inputs=operands)


def cross_below(**operands: PortRef) -> Signal:
    """Death cross signal — fires when first operand crosses below second."""
    if len(operands) != 2:
        raise ValueError("cross_below requires exactly 2 named operands")
    return Signal(op="CROSS_BELOW", inputs=operands)


def threshold(signal: PortRef, value: float) -> Signal:
    """Threshold signal — fires when signal exceeds value."""
    return Signal(
        op="THRESHOLD",
        inputs={"signal": signal},
        params={"threshold": value},
    )


def and_signal(a: PortRef, b: PortRef) -> Signal:
    """Logical AND of two signals."""
    return Signal(op="AND", inputs={"a": a, "b": b})


def or_signal(a: PortRef, b: PortRef) -> Signal:
    """Logical OR of two signals."""
    return Signal(op="OR", inputs={"a": a, "b": b})


# ── Strategy decorator and base class ──

def strategy(name: str):
    """Decorator to register a strategy class with a name."""
    def decorator(cls):
        cls._strategy_name = name
        return cls
    return decorator


class Strategy:
    """Base class for strategy definitions.

    Subclasses declare Factor and Signal nodes as class attributes,
    and define signal handlers with @on_signal decorator.
    """
    _strategy_name: str = ""

    def get_nodes(self) -> list[Factor | Signal]:
        """Extract all Factor/Signal nodes from the class."""
        nodes = []
        for attr_name in dir(self.__class__):
            attr = getattr(self.__class__, attr_name)
            if isinstance(attr, (Factor, Signal)):
                nodes.append(attr)
        return nodes


def on_signal(node_name: str):
    """Decorator for signal handler methods.

    Usage:
        @on_signal("signal")
        def handle_signal(self, ctx):
            if ctx.signal > 0:
                ctx.order(side=BUY, weight=0.95)
    """
    def decorator(method):
        method._signal_node = node_name
        return method
    return decorator


# ── Order side constants ──

BUY = 1
SELL = -1
