# ir_compiler.py — Compile DSL StrategyBase to IR JSON
#
# Takes a StrategyBase subclass, calls build() to get the computation
# graph (OpNode list), then produces a JSON string matching the C++
# StrategyGraph structure (NodeDef, EdgeDef, DataBinding).

from __future__ import annotations

import json
from typing import Any

from dsl.strategy_base import OpNode, StrategyBase


class IRCompiler:
    """Compile a StrategyBase subclass to IR JSON compatible with the C++ engine.

    Usage:
        ir_json = IRCompiler().compile(MACross)
        with open("strategy.graph", "w") as f:
            f.write(ir_json)
    """

    def compile(self, strategy_cls: type[StrategyBase]) -> str:
        """Compile a strategy class to a JSON string.

        Args:
            strategy_cls: A StrategyBase subclass with build() defined.

        Returns:
            IR JSON string matching the C++ StrategyGraph schema.
        """
        instance = strategy_cls()
        nodes = instance.build()

        strategy_name = getattr(strategy_cls, "_strategy_name", strategy_cls.__name__)

        # Convert OpNodes to NodeDef dicts (matching C++ ir_graph.h)
        node_defs = []
        for node in nodes:
            node_defs.append(self._to_node_def(node))

        # Infer edges from node inputs referencing other nodes
        edges = self._infer_edges(nodes)

        # Infer data bindings from node inputs referencing data fields
        data_bindings = self._infer_data_bindings(nodes)

        graph = {
            "strategy_name": strategy_name,
            "version": 1,
            "nodes": node_defs,
            "edges": edges,
            "data_bindings": data_bindings,
            "signal_handlers": [],
        }

        # Validate before returning
        errors = self._validate(graph)
        if errors:
            raise ValueError(f"IR validation failed: {'; '.join(errors)}")

        return json.dumps(graph, indent=2)

    def compile_to_dict(self, strategy_cls: type[StrategyBase]) -> dict:
        """Compile to a dict (useful for programmatic inspection)."""
        instance = strategy_cls()
        strategy_name = getattr(strategy_cls, "_strategy_name", strategy_cls.__name__)
        nodes = instance.build()

        return {
            "strategy_name": strategy_name,
            "version": 1,
            "nodes": [self._to_node_def(n) for n in nodes],
            "edges": self._infer_edges(nodes),
            "data_bindings": self._infer_data_bindings(nodes),
            "signal_handlers": [],
        }

    # ── Internal helpers ──

    def _to_node_def(self, node: OpNode) -> dict:
        """Convert an OpNode to a NodeDef dict matching C++ ir_graph.h."""
        # Build input port dict
        input_ports: dict[str, dict] = {}
        for port_name, source in node.inputs.items():
            if source["type"] == "data":
                input_ports[port_name] = {
                    "name": port_name,
                    "type": {"base_type": "TimeSeries", "inner_type": "float"},
                    "source": source["field"],
                }
            elif source["type"] == "node":
                ref = f"node.{source['node_id']}.{source['port']}"
                input_ports[port_name] = {
                    "name": port_name,
                    "type": {"base_type": "TimeSeries", "inner_type": "float"},
                    "source": ref,
                }

        return {
            "id": node.id,
            "op_type": node.op_type,
            "inputs": input_ports,
            "outputs": dict(node.outputs) if node.outputs else {},
            "params": dict(node.params) if node.params else {},
        }

    def _infer_edges(self, nodes: list[OpNode]) -> list[dict]:
        """Infer edges from node inputs that reference other nodes."""
        edges: list[dict] = []
        for node in nodes:
            for port_name, source in node.inputs.items():
                if source["type"] == "node":
                    edges.append({
                        "from_node": source["node_id"],
                        "from_port": source["port"],
                        "to_node": node.id,
                        "to_port": port_name,
                    })
        return edges

    def _infer_data_bindings(self, nodes: list[OpNode]) -> list[dict]:
        """Infer data bindings from node inputs that reference data fields."""
        bindings: list[dict] = []
        for node in nodes:
            for port_name, source in node.inputs.items():
                if source["type"] == "data":
                    bindings.append({
                        "data_source": source["field"],
                        "to_node": node.id,
                        "to_port": port_name,
                    })
        return bindings

    def _validate(self, graph: dict) -> list[str]:
        """Validate graph topology and completeness.

        Checks:
          - All node IDs are unique
          - Edge references point to existing nodes
          - Data binding references point to existing nodes
          - No cycles in the graph
        """
        errors: list[str] = []
        node_ids = {n["id"] for n in graph["nodes"]}

        # Check unique IDs
        if len(node_ids) != len(graph["nodes"]):
            errors.append("Duplicate node IDs detected")

        # Check edge references
        for edge in graph["edges"]:
            if edge["from_node"] not in node_ids:
                errors.append(f"Edge references unknown from_node: {edge['from_node']}")
            if edge["to_node"] not in node_ids:
                errors.append(f"Edge references unknown to_node: {edge['to_node']}")

        # Check data binding references
        for binding in graph["data_bindings"]:
            if binding["to_node"] not in node_ids:
                errors.append(
                    f"DataBinding references unknown node: {binding['to_node']}"
                )

        # Cycle detection via DFS (skip edges with unknown nodes — already reported above)
        adj: dict[str, list[str]] = {nid: [] for nid in node_ids}
        for edge in graph["edges"]:
            if edge["from_node"] in adj:
                adj[edge["from_node"]].append(edge["to_node"])

        VISITING, VISITED = 1, 2
        state: dict[str, int] = {}

        def has_cycle(nid: str) -> bool:
            state[nid] = VISITING
            for dep in adj.get(nid, []):
                if dep in state:
                    if state[dep] == VISITING:
                        return True
                elif has_cycle(dep):
                    return True
            state[nid] = VISITED
            return False

        for nid in node_ids:
            if nid not in state and has_cycle(nid):
                errors.append(f"Cycle detected involving node: {nid}")
                break

        return errors
