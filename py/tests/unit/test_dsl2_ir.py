# test_dsl2_ir.py — Tests for DSL v2 and IR compiler

import json
import tempfile
from pathlib import Path

import pytest

from quant_invest.strategy.dsl2 import (
    BUY,
    SELL,
    DataField,
    Factor,
    PortRef,
    Signal,
    Strategy,
    TS_FLOAT,
    SIGNAL_FLOAT,
    cross_above,
    cross_below,
    on_signal,
    strategy,
    threshold,
)
from quant_invest.strategy.ir_compiler import IRCompiler


# ── Test: Factor creation and typed I/O ──

class TestFactor:
    def test_factor_output_ref(self):
        f = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        ref = f.output("value")
        assert isinstance(ref, PortRef)
        assert ref.port_type == TS_FLOAT

    def test_factor_to_node_def(self):
        f = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        f._node_id = "fast_ma"
        node_def = f.to_node_def()
        assert node_def["id"] == "fast_ma"
        assert node_def["op_type"] == "SMA"
        assert node_def["params"]["period"] == 5.0
        assert "price" in node_def["inputs"]
        assert node_def["inputs"]["price"]["source"] == "kline.close"

    def test_factor_set_name(self):
        class MyStrat(Strategy):
            fast_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})

        assert MyStrat.fast_ma._node_id == "fast_ma"


# ── Test: Signal combinators ──

class TestSignalCombinators:
    def test_cross_above(self):
        f1 = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        f1._node_id = "fast"
        f2 = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 20})
        f2._node_id = "slow"

        sig = cross_above(fast=f1.output("value"), slow=f2.output("value"))
        assert isinstance(sig, Signal)
        assert sig.op == "CROSS_ABOVE"
        assert "fast" in sig.inputs
        assert "slow" in sig.inputs

    def test_cross_below(self):
        f1 = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        f1._node_id = "fast"
        f2 = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 20})
        f2._node_id = "slow"

        sig = cross_below(fast=f1.output("value"), slow=f2.output("value"))
        assert sig.op == "CROSS_BELOW"

    def test_threshold(self):
        f = Factor(op="RSI", inputs={"price": DataField.close}, params={"period": 14})
        f._node_id = "rsi"
        sig = threshold(f.output("value"), 70.0)
        assert sig.op == "THRESHOLD"
        assert sig.params["threshold"] == 70.0

    def test_cross_above_requires_two_operands(self):
        with pytest.raises(ValueError, match="exactly 2"):
            cross_above(fast=PortRef("a", "v", TS_FLOAT))


# ── Test: IR Compiler ──

class TestIRCompiler:
    def test_compile_ma_cross(self):
        # Create factors outside the class body to avoid NameError
        fast_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        slow_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 20})
        signal = cross_above(fast=fast_ma.output("value"), slow=slow_ma.output("value"))

        @strategy("ma_cross")
        class MACross(Strategy):
            fast_ma = fast_ma
            slow_ma = slow_ma
            signal = signal

        compiler = IRCompiler()
        ir = compiler.compile(MACross)

        assert ir["strategy_name"] == "ma_cross"
        assert ir["version"] == 1
        assert len(ir["nodes"]) == 3
        assert len(ir["edges"]) == 2
        assert len(ir["data_bindings"]) == 2

        # Check edges
        edge_pairs = {(e["from_node"], e["to_node"]) for e in ir["edges"]}
        assert ("fast_ma", "signal") in edge_pairs
        assert ("slow_ma", "signal") in edge_pairs

        # Check data bindings
        binding_nodes = {b["to_node"] for b in ir["data_bindings"]}
        assert "fast_ma" in binding_nodes
        assert "slow_ma" in binding_nodes

    def test_write_graph_file(self):
        @strategy("test_write")
        class TestStrat(Strategy):
            ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 10})

        compiler = IRCompiler()
        with tempfile.NamedTemporaryFile(suffix=".graph", delete=False, mode="w") as f:
            path = f.name

        compiler.write_graph(TestStrat, path)

        with open(path) as f:
            ir = json.load(f)

        assert ir["strategy_name"] == "test_write"
        assert len(ir["nodes"]) == 1

        Path(path).unlink()

    def test_round_trip_with_cpp_ir(self):
        """Test that Python IR output is valid for C++ IR parser."""
        fast_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        slow_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 20})
        signal = cross_above(fast=fast_ma.output("value"), slow=slow_ma.output("value"))

        @strategy("round_trip")
        class RoundTrip(Strategy):
            fast_ma = fast_ma
            slow_ma = slow_ma
            signal = signal

            @on_signal("signal")
            def handle_signal(self, ctx):
                pass

        compiler = IRCompiler()
        ir = compiler.compile(RoundTrip)

        # Verify JSON is valid
        json_str = json.dumps(ir, indent=2)
        ir2 = json.loads(json_str)
        assert ir2["strategy_name"] == "round_trip"
        assert len(ir2["nodes"]) == 3

        # Verify structure matches C++ IR expectations
        for node in ir2["nodes"]:
            assert "id" in node
            assert "op_type" in node
            assert "inputs" in node
            assert "outputs" in node
            assert "params" in node

    def test_multiple_signals(self):
        fast_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 5})
        slow_ma = Factor(op="SMA", inputs={"price": DataField.close}, params={"period": 20})
        golden = cross_above(fast=fast_ma.output("value"), slow=slow_ma.output("value"))
        death = cross_below(fast=fast_ma.output("value"), slow=slow_ma.output("value"))

        @strategy("multi_signal")
        class MultiSignal(Strategy):
            fast_ma = fast_ma
            slow_ma = slow_ma
            golden = golden
            death = death

        compiler = IRCompiler()
        ir = compiler.compile(MultiSignal)

        assert len(ir["nodes"]) == 4
        assert len(ir["edges"]) == 4
        ops = {n["op_type"] for n in ir["nodes"]}
        assert "CROSS_ABOVE" in ops
        assert "CROSS_BELOW" in ops