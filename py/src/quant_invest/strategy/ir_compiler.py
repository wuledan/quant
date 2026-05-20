# ir_compiler.py — Compile DSL v2 strategies to IR (.graph JSON)
#
# Takes a Strategy class with Factor/Signal declarations and produces
# a StrategyGraph IR JSON file that the C++ backend can load and execute.

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from quant_invest.strategy.dsl2 import (
    DataField,
    Factor,
    PortRef,
    Signal,
    Strategy,
    TypeSpec,
)


class IRCompiler:
    """Compile a Strategy class to IR (.graph JSON).

    Usage:
        compiler = IRCompiler()
        compiler.write_graph(MACross, "ma_cross.graph")
    """

    def compile(self, strategy_cls: type[Strategy]) -> dict:
        """Compile a Strategy class to an IR dict."""
        name = getattr(strategy_cls, "_strategy_name", strategy_cls.__name__)
        instance = strategy_cls()
        nodes = instance.get_nodes()

        # Build node definitions
        node_defs = []
        for node in nodes:
            node_defs.append(node.to_node_def())

        # Infer edges from Signal.inputs (PortRef → edge)
        edges = self._infer_edges(nodes)

        # Infer data bindings from Factor.inputs (DataField → binding)
        data_bindings = self._infer_data_bindings(nodes)

        # Extract signal handlers from @on_signal methods
        signal_handlers = self._extract_handlers(strategy_cls)

        return {
            "strategy_name": name,
            "version": 1,
            "nodes": node_defs,
            "edges": edges,
            "data_bindings": data_bindings,
            "signal_handlers": signal_handlers,
        }

    def write_graph(self, strategy_cls: type[Strategy], output_path: str) -> str:
        """Compile and write to .graph file."""
        ir = self.compile(strategy_cls)
        with open(output_path, "w") as f:
            json.dump(ir, f, indent=2)
        return output_path

    def _infer_edges(self, nodes: list[Factor | Signal]) -> list[dict]:
        """Generate edges from Signal.inputs PortRefs."""
        edges = []
        for node in nodes:
            if isinstance(node, Signal):
                for port_name, port_ref in node.inputs.items():
                    if isinstance(port_ref, PortRef):
                        edges.append({
                            "from_node": port_ref.node_id,
                            "from_port": port_ref.port_name,
                            "to_node": node._node_id,
                            "to_port": port_name,
                        })
        return edges

    def _infer_data_bindings(self, nodes: list[Factor | Signal]) -> list[dict]:
        """Generate data bindings from Factor.inputs DataFields."""
        bindings = []
        for node in nodes:
            if isinstance(node, Factor):
                for port_name, source in node.inputs.items():
                    # DataField is a class with string attributes
                    if isinstance(source, str):
                        bindings.append({
                            "data_source": source,
                            "to_node": node._node_id,
                            "to_port": port_name,
                        })
        return bindings

    def _extract_handlers(self, strategy_cls: type[Strategy]) -> list[dict]:
        """Extract @on_signal handler definitions."""
        handlers = []
        for attr_name in dir(strategy_cls):
            method = getattr(strategy_cls, attr_name)
            if callable(method) and hasattr(method, "_signal_node"):
                # Extract handler params from method source or defaults
                # For now, use default order handler params
                handlers.append({
                    "signal_node": method._signal_node,
                    "handler_type": "order",
                    "params": {},
                })
        return handlers
