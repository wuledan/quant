# test_ir_compiler.py — Tests for standalone DSL → IR compilation
#
# Verifies that StrategyBase subclasses compile to correct IR JSON
# matching the C++ StrategyGraph schema defined in ir_graph.h.

from __future__ import annotations

import json
import re

import pytest

from compiler.ir_compiler import IRCompiler
from dsl.strategy_base import (
    StrategyBase,
    cross_above,
    cross_below,
    ema,
    sma,
    threshold,
)


# ═══════════════════════════════════════════════════════════════════
# MACross strategy — single signal
# ═══════════════════════════════════════════════════════════════════

class MACross(StrategyBase):
    """Simple MA crossover strategy."""
    fast_period = 5
    slow_period = 20

    def build(self):
        fast = sma("kline.close", self.fast_period)
        slow = sma("kline.close", self.slow_period)
        sig = cross_above(fast, slow)
        return [fast, slow, sig]


class TestMACrossCompilation:
    """Verify that a simple MACross strategy compiles to correct IR JSON."""

    def test_compiles_without_error(self):
        """Smoke test: compilation does not raise."""
        compiler = IRCompiler()
        ir_json = compiler.compile(MACross)
        assert isinstance(ir_json, str)

    def test_json_is_valid(self):
        """Output is valid JSON."""
        compiler = IRCompiler()
        ir = json.loads(compiler.compile(MACross))
        assert isinstance(ir, dict)

    def test_strategy_name(self):
        """Strategy name is derived from class name."""
        ir = json.loads(IRCompiler().compile(MACross))
        assert ir["strategy_name"] == "MACross"

    def test_version(self):
        ir = json.loads(IRCompiler().compile(MACross))
        assert ir["version"] == 1

    def test_three_nodes(self):
        """MACross has 3 nodes: fast SMA, slow SMA, cross_above signal."""
        ir = json.loads(IRCompiler().compile(MACross))
        assert len(ir["nodes"]) == 3

    def test_two_edges(self):
        """Two edges: fast→signal, slow→signal."""
        ir = json.loads(IRCompiler().compile(MACross))
        assert len(ir["edges"]) == 2

    def test_two_data_bindings(self):
        """Both SMAs bind to kline.close."""
        ir = json.loads(IRCompiler().compile(MACross))
        assert len(ir["data_bindings"]) == 2

    def test_no_signal_handlers(self):
        ir = json.loads(IRCompiler().compile(MACross))
        assert ir["signal_handlers"] == []

    def test_node_structure(self):
        """Each node has id, op_type, inputs, outputs, params."""
        ir = json.loads(IRCompiler().compile(MACross))
        for node in ir["nodes"]:
            assert "id" in node
            assert "op_type" in node
            assert "inputs" in node
            assert "outputs" in node
            assert "params" in node

    def test_fast_ma_node(self):
        ir = json.loads(IRCompiler().compile(MACross))
        nodes = {n["id"]: n for n in ir["nodes"]}
        fast = [n for nid, n in nodes.items() if "sma" in nid][0]
        # Since auto-generated IDs start from 1 in a fresh interpreter,
        # assert on structure rather than exact ID
        assert fast["op_type"] == "SMA"
        assert fast["params"]["period"] == 5.0
        assert "price" in fast["inputs"]
        assert "value" in fast["outputs"]

    def test_slow_ma_node(self):
        ir = json.loads(IRCompiler().compile(MACross))
        slow = [n for n in ir["nodes"] if n["op_type"] == "SMA" and n["params"]["period"] == 20.0][0]
        assert slow["params"]["period"] == 20.0

    def test_signal_node(self):
        ir = json.loads(IRCompiler().compile(MACross))
        signal = [n for n in ir["nodes"] if n["op_type"] == "CROSS_ABOVE"][0]
        assert "fast" in signal["inputs"]
        assert "slow" in signal["inputs"]
        assert "signal" in signal["outputs"]

    def test_edge_targets_exist(self):
        """All edge endpoints reference real nodes."""
        ir = json.loads(IRCompiler().compile(MACross))
        node_ids = {n["id"] for n in ir["nodes"]}
        for edge in ir["edges"]:
            assert edge["from_node"] in node_ids
            assert edge["to_node"] in node_ids

    def test_data_binding_nodes_exist(self):
        ir = json.loads(IRCompiler().compile(MACross))
        node_ids = {n["id"] for n in ir["nodes"]}
        for binding in ir["data_bindings"]:
            assert binding["to_node"] in node_ids

    def test_well_known_json_format(self):
        """Ensure output structure matches the expected MACross example."""
        ir = json.loads(IRCompiler().compile(MACross))
        # Verify the data source
        for binding in ir["data_bindings"]:
            assert binding["data_source"] == "kline.close"

        # Verify input sources for signal node
        signal = [n for n in ir["nodes"] if n["op_type"] == "CROSS_ABOVE"][0]
        fast_input = signal["inputs"]["fast"]
        slow_input = signal["inputs"]["slow"]
        assert fast_input["source"].startswith("node.")
        assert slow_input["source"].startswith("node.")


# ═══════════════════════════════════════════════════════════════════
# Multi-factor strategy
# ═══════════════════════════════════════════════════════════════════

class MultiFactorStrategy(StrategyBase):
    """Strategy with multiple factors and both cross_above/cross_below."""
    fast_period = 10
    slow_period = 30
    rsi_period = 14

    def build(self):
        fast = ema("kline.close", self.fast_period)
        slow = ema("kline.close", self.slow_period)
        rsi_val = sma("kline.close", self.rsi_period)
        golden = cross_above(fast, slow)
        death = cross_below(fast, slow)
        return [fast, slow, rsi_val, golden, death]


class TestMultiFactorCompilation:
    """Multi-factor strategy with 5 nodes and 4 edges."""

    def test_five_nodes(self):
        ir = json.loads(IRCompiler().compile(MultiFactorStrategy))
        assert len(ir["nodes"]) == 5

    def test_four_edges(self):
        """golden uses fast+slow, death uses fast+slow = 4 edges."""
        ir = json.loads(IRCompiler().compile(MultiFactorStrategy))
        assert len(ir["edges"]) == 4

    def test_three_data_bindings(self):
        """fast EMA, slow EMA, rsi SMA all bind to kline.close."""
        ir = json.loads(IRCompiler().compile(MultiFactorStrategy))
        assert len(ir["data_bindings"]) == 3

    def test_op_types_present(self):
        ir = json.loads(IRCompiler().compile(MultiFactorStrategy))
        ops = {n["op_type"] for n in ir["nodes"]}
        assert "EMA" in ops
        assert "SMA" in ops
        assert "CROSS_ABOVE" in ops
        assert "CROSS_BELOW" in ops

    def test_signal_edges_use_ema_nodes(self):
        """Cross signals connect to EMA nodes, not the SMA RSI."""
        ir = json.loads(IRCompiler().compile(MultiFactorStrategy))
        ema_ids = {n["id"] for n in ir["nodes"] if n["op_type"] == "EMA"}
        for edge in ir["edges"]:
            assert edge["from_node"] in ema_ids


# ═══════════════════════════════════════════════════════════════════
# Threshold strategy
# ═══════════════════════════════════════════════════════════════════

class ThresholdStrategy(StrategyBase):
    threshold_level = 70.0

    def build(self):
        rsi_val = sma("kline.close", 14)
        sig = threshold(rsi_val, self.threshold_level)
        return [rsi_val, sig]


class TestThresholdCompilation:
    """Strategy with threshold operator."""

    def test_two_nodes(self):
        ir = json.loads(IRCompiler().compile(ThresholdStrategy))
        assert len(ir["nodes"]) == 2

    def test_threshold_node(self):
        ir = json.loads(IRCompiler().compile(ThresholdStrategy))
        thresh = [n for n in ir["nodes"] if n["op_type"] == "THRESHOLD"][0]
        assert thresh["params"]["threshold"] == 70.0
        assert "input" in thresh["inputs"]
        assert "signal" in thresh["outputs"]

    def test_one_edge(self):
        ir = json.loads(IRCompiler().compile(ThresholdStrategy))
        assert len(ir["edges"]) == 1


# ═══════════════════════════════════════════════════════════════════
# Validation tests
# ═══════════════════════════════════════════════════════════════════

class TestValidation:
    """IR graph validation."""

    def test_strategy_name_override(self):
        """_strategy_name class attribute overrides the class name."""
        class Renamed(StrategyBase):
            _strategy_name = "MyCustomName"

            def build(self):
                return [sma("kline.close", 5)]

        ir = json.loads(IRCompiler().compile(Renamed))
        assert ir["strategy_name"] == "MyCustomName"

    def test_cycle_detection(self):
        """Compiler correctly rejects graphs with cycles."""
        # A self-referencing input would produce a cycle (node→node)
        # but our operators are DAG by construction. We test the validator.
        compiler = IRCompiler()
        bad_graph = {
            "strategy_name": "cyclic",
            "version": 1,
            "nodes": [
                {"id": "a", "op_type": "SMA", "inputs": {}, "outputs": {}, "params": {}},
                {"id": "b", "op_type": "SMA", "inputs": {}, "outputs": {}, "params": {}},
            ],
            "edges": [
                {"from_node": "a", "from_port": "value", "to_node": "b", "to_port": "price"},
                {"from_node": "b", "from_port": "value", "to_node": "a", "to_port": "price"},
            ],
            "data_bindings": [],
            "signal_handlers": [],
        }
        errors = compiler._validate(bad_graph)
        assert any("Cycle" in e for e in errors)

    def test_bad_edge_reference(self):
        compiler = IRCompiler()
        bad_graph = {
            "strategy_name": "bad_edge",
            "version": 1,
            "nodes": [
                {"id": "a", "op_type": "SMA", "inputs": {}, "outputs": {}, "params": {}},
            ],
            "edges": [
                {"from_node": "nonexistent", "from_port": "x", "to_node": "a", "to_port": "y"},
            ],
            "data_bindings": [],
            "signal_handlers": [],
        }
        errors = compiler._validate(bad_graph)
        assert any("nonexistent" in e for e in errors)

    def test_duplicate_node_ids(self):
        """Duplicate IDs are detected."""
        compiler = IRCompiler()
        bad_graph = {
            "strategy_name": "dup",
            "version": 1,
            "nodes": [
                {"id": "a", "op_type": "SMA", "inputs": {}, "outputs": {}, "params": {}},
                {"id": "a", "op_type": "EMA", "inputs": {}, "outputs": {}, "params": {}},
            ],
            "edges": [],
            "data_bindings": [],
            "signal_handlers": [],
        }
        errors = compiler._validate(bad_graph)
        assert any("Duplicate" in e for e in errors)


# ═══════════════════════════════════════════════════════════════════
# Round-trip compatibility with C++ IR schema
# ═══════════════════════════════════════════════════════════════════

class TestIRSchemaCompatibility:
    """Verify the output matches C++ StrategyGraph expectations."""

    def test_top_level_keys(self):
        """Dict has all expected top-level keys."""
        ir = json.loads(IRCompiler().compile(MACross))
        expected_keys = {
            "strategy_name", "version", "nodes",
            "edges", "data_bindings", "signal_handlers",
        }
        assert set(ir.keys()) == expected_keys

    def test_node_has_all_fields(self):
        """Each node dict has all fields the C++ side expects."""
        ir = json.loads(IRCompiler().compile(MACross))
        for node in ir["nodes"]:
            assert set(node.keys()) == {"id", "op_type", "inputs", "outputs", "params"}

    def test_port_has_type_spec(self):
        """Each input/output port has name, type (with base_type + inner_type), source."""
        ir = json.loads(IRCompiler().compile(MACross))
        for node in ir["nodes"]:
            for port_name, port in node["inputs"].items():
                assert "name" in port
                assert "type" in port
                assert "source" in port
                assert "base_type" in port["type"]
                assert "inner_type" in port["type"]

    def test_edge_has_all_fields(self):
        ir = json.loads(IRCompiler().compile(MACross))
        for edge in ir["edges"]:
            assert set(edge.keys()) == {"from_node", "from_port", "to_node", "to_port"}

    def test_data_binding_has_all_fields(self):
        ir = json.loads(IRCompiler().compile(MACross))
        for binding in ir["data_bindings"]:
            assert set(binding.keys()) == {"data_source", "to_node", "to_port"}

    def test_json_round_trip(self):
        """Serialized JSON can be parsed back."""
        json_str = IRCompiler().compile(MACross)
        parsed = json.loads(json_str)
        assert parsed["strategy_name"] == "MACross"
        assert len(parsed["nodes"]) == 3


# ═══════════════════════════════════════════════════════════════════
# Parameter passing
# ═══════════════════════════════════════════════════════════════════

class TestParameterExtraction:
    """StrategyBase should correctly expose user-defined parameters."""

    def test_get_params(self):
        class ParamStrategy(StrategyBase):
            fast_period = 5
            slow_period = 20
            name = "test"

            def build(self):
                return []

        instance = ParamStrategy()
        params = instance.get_params()
        assert params["fast_period"] == 5
        assert params["slow_period"] == 20
        assert params["name"] == "test"
        # Methods are excluded
        assert "build" not in params
        assert "get_params" not in params


# ═══════════════════════════════════════════════════════════════════
# Multiple instances / reentrancy
# ═══════════════════════════════════════════════════════════════════

class TestReentrancy:
    """Compiler is stateless and can be called multiple times."""

    def test_two_calls_produce_same_structure(self):
        ir1 = json.loads(IRCompiler().compile(MACross))
        ir2 = json.loads(IRCompiler().compile(MACross))
        assert ir1["strategy_name"] == ir2["strategy_name"]
        assert len(ir1["nodes"]) == len(ir2["nodes"])
        assert len(ir1["edges"]) == len(ir2["edges"])

    def test_two_different_strategies(self):
        class StrategyA(StrategyBase):
            def build(self):
                return [sma("kline.close", 5)]

        class StrategyB(StrategyBase):
            def build(self):
                return [sma("kline.high", 10)]

        ir_a = json.loads(IRCompiler().compile(StrategyA))
        ir_b = json.loads(IRCompiler().compile(StrategyB))

        assert ir_a["strategy_name"] == "StrategyA"
        assert ir_b["strategy_name"] == "StrategyB"
        assert ir_a["nodes"][0]["params"]["period"] == 5.0
        assert ir_b["nodes"][0]["params"]["period"] == 10.0
