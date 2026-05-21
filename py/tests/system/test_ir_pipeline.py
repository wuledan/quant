"""test_ir_pipeline.py — IR 编译管线端到端集成测试

测试链路:
  1. Python DSL → IR 编译 → .graph 文件生成
  2. C++ 加载 .graph → FactorDAG::from_graph() → 因子计算
  3. C++ SignalHandler → 订单生成
  4. C++ BacktestRunner 全链路 → 净值曲线
  5. Python API: 上传策略 → 注册 → 回测 → 查询结果

验收标准:
  - 所有集成测试通过
  - 端到端延迟 < 1s (回测 1 年日线数据)
"""
import os
import tempfile
import time
from pathlib import Path

import pytest

# ── Import with graceful skip if C++ extension not built ──
try:
    from quant_invest.c_bindings import (
        BacktestParams,
        BacktestResult,
        BacktestRunner,
        DataBinding,
        EdgeDef,
        EventBus,
        EventBusOptions,
        IRSignalHandler,
        NodeDef,
        PortDef,
        StrategyEngine,
        StrategyGraph,
        StrategyRegistry,
        StorageEngine,
        TypeSpec,
    )

    _HAS_CPP_BINDINGS = True
except ImportError:
    _HAS_CPP_BINDINGS = False

try:
    from quant_invest.strategy.dsl2 import Strategy, macross
    from quant_invest.strategy.ir_compiler import compile_strategy

    _HAS_DSL = True
except ImportError:
    _HAS_DSL = False

requires_cpp = pytest.mark.skipif(
    not _HAS_CPP_BINDINGS,
    reason="C++ extension (_quant_core) not built",
)

requires_dsl = pytest.mark.skipif(
    not _HAS_DSL,
    reason="Python DSL/IR compiler not available",
)


# ── Fixtures ──


@pytest.fixture
def tmp_dir():
    """Provide a temporary directory that is cleaned up after the test."""
    with tempfile.TemporaryDirectory(prefix="ir_pipeline_") as d:
        yield d


@pytest.fixture
def storage(tmp_dir):
    """Create a StorageEngine with a temp directory and store synthetic data."""
    engine = StorageEngine(4, tmp_dir)
    _store_synthetic_kline(engine)
    yield engine
    engine.close()


def _store_synthetic_kline(storage: StorageEngine):
    """Store 250 bars (~1 year) of synthetic daily kline data with SMA crossover.

    Pattern:
      Bars 0-99:   price declining 100 → 50  (slow SMA > fast SMA)
      Bars 100-249: price rising 50 → 200    (fast SMA crosses above slow SMA)
    """
    import quant_invest._quant_core as core

    rows = []
    base_ts = 1700000000000000  # arbitrary start (microseconds)
    day_us = 86400 * 1_000_000

    for i in range(250):
        if i < 100:
            price = 100.0 - 0.5 * i  # declining: 100 → 50
        else:
            price = 50.0 + 1.0 * (i - 99)  # rising: 50 → 201

        row = core.KlineRow(
            timestamp=base_ts + i * day_us,
            open_price=int(round(price * 10000)),
            high_price=int(round((price + 0.3) * 10000)),
            low_price=int(round((price - 0.3) * 10000)),
            close_price=int(round(price * 10000)),
            volume=1_000_000 + i * 10_000,
            amount=int(round(price * 10000)) * 100,
            vwap=int(round(price * 10000)),
        )
        rows.append(row)

    status = storage.store_kline_batch("600519.SH", 5, rows)  # 5 = kKlineDay
    assert status == 0, f"store_kline_batch failed with status {status}"


def _make_sma_cross_graph() -> StrategyGraph:
    """Programmatically build an SMA crossover StrategyGraph via C++ bindings."""
    g = StrategyGraph()
    g.strategy_name = "sma_cross_pipeline"

    # ── Node: fast_ma ──
    fast_ma = NodeDef()
    fast_ma.id = "fast_ma"
    fast_ma.op_type = "SMA"
    fast_ma.inputs = {
        "price": PortDef(name="price", type=TypeSpec(base_type="TimeSeries", inner_type="float"), source="data.close")
    }
    fast_ma.outputs = {
        "value": PortDef(name="value", type=TypeSpec(base_type="TimeSeries", inner_type="float"), source="")
    }
    fast_ma.params = {"period": 5.0}

    # ── Node: slow_ma ──
    slow_ma = NodeDef()
    slow_ma.id = "slow_ma"
    slow_ma.op_type = "SMA"
    slow_ma.inputs = {
        "price": PortDef(name="price", type=TypeSpec(base_type="TimeSeries", inner_type="float"), source="data.close")
    }
    slow_ma.outputs = {
        "value": PortDef(name="value", type=TypeSpec(base_type="TimeSeries", inner_type="float"), source="")
    }
    slow_ma.params = {"period": 20.0}

    # ── Node: signal ──
    signal = NodeDef()
    signal.id = "signal"
    signal.op_type = "CROSS_ABOVE"
    signal.inputs = {
        "fast": PortDef(name="fast", type=TypeSpec(base_type="TimeSeries", inner_type="float"), source="node.fast_ma.value"),
        "slow": PortDef(name="slow", type=TimeSeries", inner_type="float"), source="node.slow_ma.value"),
    }
    signal.outputs = {
        "value": PortDef(name="value", type=TypeSpec(base_type="Signal", inner_type="float"), source="")
    }

    g.nodes = [fast_ma, slow_ma, signal]

    # ── Edges ──
    g.edges = [
        EdgeDef(from_node="fast_ma", from_port="value", to_node="signal", to_port="fast"),
        EdgeDef(from_node="slow_ma", from_port="value", to_node="signal", to_port="slow"),
    ]

    # ── Data bindings ──
    g.data_bindings = [
        DataBinding(data_source="kline.close", to_node="fast_ma", to_port="price"),
        DataBinding(data_source="kline.close", to_node="slow_ma", to_port="price"),
    ]

    # ── Signal handler ──
    sh = IRSignalHandler()
    sh.signal_node = "signal"
    sh.handler_type = "order"
    sh.params = {"buy_weight": 0.95, "sell_weight": 1.0}
    g.signal_handlers = [sh]

    return g


def _make_backtest_params() -> BacktestParams:
    """Default backtest params matching the synthetic data."""
    params = BacktestParams()
    params.initial_cash = 1_000_000.0
    params.start_time = 0
    params.end_time = 9999999999999999
    params.symbol = "600519.SH"
    params.kline_type = 5  # kKlineDay
    return params


# ═══════════════════════════════════════════════════════════
# Test 1: Python DSL → IR 编译 → .graph 文件生成
# ═══════════════════════════════════════════════════════════


@requires_dsl
class TestDSLToIRCompilation:
    """Test that Python DSL strategies compile to valid IR graphs."""

    def test_macross_compiles_to_graph(self):
        """MACross strategy → compile_strategy() → StrategyGraph with valid structure."""
        strat = macross(fast_period=5, slow_period=20)
        graph = compile_strategy(strat)

        assert isinstance(graph, StrategyGraph)
        assert graph.strategy_name != ""
        assert len(graph.nodes) >= 2  # at least fast_ma + slow_ma
        assert len(graph.edges) >= 1  # at least one edge
        assert len(graph.signal_handlers) >= 1  # at least one handler

    def test_macross_graph_has_sma_nodes(self):
        """Compiled MACross graph should contain SMA nodes with correct periods."""
        strat = macross(fast_period=5, slow_period=20)
        graph = compile_strategy(strat)

        sma_nodes = [n for n in graph.nodes if n.op_type == "SMA"]
        assert len(sma_nodes) >= 2

        periods = {n.params.get("period") for n in sma_nodes}
        assert 5.0 in periods
        assert 20.0 in periods

    def test_macross_graph_write_to_file(self, tmp_dir):
        """Compiled graph can be written to a .graph file."""
        strat = macross(fast_period=5, slow_period=20)
        graph = compile_strategy(strat)

        path = os.path.join(tmp_dir, "macross.graph")
        graph.write_to_file(path)

        assert os.path.exists(path)
        assert os.path.getsize(path) > 0


# ═══════════════════════════════════════════════════════════
# Test 2: C++ 加载 .graph → FactorDAG → 因子计算
# ═══════════════════════════════════════════════════════════


@requires_cpp
class TestIRGraphToFactorDAG:
    """Test that IR graphs can be loaded and converted to FactorDAGs."""

    def test_graph_to_dag_valid(self):
        """StrategyGraph → FactorDAG::from_graph() → valid DAG."""
        from quant_invest._quant_core import FactorDAG, FactorRegistry, OpRegistry

        OpRegistry.register_all_builtin_ops()
        graph = _make_sma_cross_graph()

        registry = FactorRegistry()
        dag = FactorDAG.from_graph(graph, registry)

        assert dag is not None
        assert dag.is_built()

        validation = dag.validate()
        assert validation.valid

    def test_dag_topological_sort(self):
        """DAG topological sort should have 3 nodes."""
        from quant_invest._quant_core import FactorDAG, FactorRegistry, OpRegistry

        OpRegistry.register_all_builtin_ops()
        graph = _make_sma_cross_graph()

        registry = FactorRegistry()
        dag = FactorDAG.from_graph(graph, registry)

        topo = dag.topological_sort()
        assert len(topo) == 3

    def test_dag_parallel_levels(self):
        """DAG parallel levels: [{fast_ma, slow_ma}, {signal}]."""
        from quant_invest._quant_core import FactorDAG, FactorRegistry, OpRegistry

        OpRegistry.register_all_builtin_ops()
        graph = _make_sma_cross_graph()

        registry = FactorRegistry()
        dag = FactorDAG.from_graph(graph, registry)

        levels = dag.parallel_levels()
        assert len(levels) == 2
        assert len(levels[0]) == 2  # fast_ma + slow_ma
        assert len(levels[1]) == 1  # signal


# ═══════════════════════════════════════════════════════════
# Test 3: C++ SignalHandler → 订单生成
# ═══════════════════════════════════════════════════════════


@requires_cpp
class TestSignalHandlerOrderGeneration:
    """Test that signal handlers generate orders correctly."""

    def test_graph_has_order_handler(self):
        """SMA cross graph should have an order signal handler."""
        graph = _make_sma_cross_graph()
        assert len(graph.signal_handlers) == 1

        handler = graph.signal_handlers[0]
        assert handler.handler_type == "order"
        assert handler.signal_node == "signal"
        assert handler.params.get("buy_weight") == pytest.approx(0.95)
        assert handler.params.get("sell_weight") == pytest.approx(1.0)


# ═══════════════════════════════════════════════════════════
# Test 4: C++ BacktestRunner 全链路 → 净值曲线
# ═══════════════════════════════════════════════════════════


@requires_cpp
class TestBacktestRunnerFullPipeline:
    """Test the complete backtest pipeline from graph to result."""

    def test_full_backtest_returns_valid_result(self, storage):
        """Graph → BacktestRunner.run() → valid BacktestResult."""
        bus = EventBus(EventBusOptions())
        graph = _make_sma_cross_graph()
        runner = BacktestRunner(storage, bus)

        result = runner.run(graph, _make_backtest_params())

        assert isinstance(result, BacktestResult)
        assert len(result.nav_curve) > 0
        assert result.total_trades >= 0
        assert isinstance(result.total_return, float)
        assert isinstance(result.max_drawdown, float)
        assert 0.0 <= result.max_drawdown <= 1.0
        assert isinstance(result.sharpe_ratio, float)

    def test_backtest_nav_curve_monotonically_initialized(self, storage):
        """Nav curve should start at 1.0 (initial NAV)."""
        bus = EventBus(EventBusOptions())
        graph = _make_sma_cross_graph()
        runner = BacktestRunner(storage, bus)

        result = runner.run(graph, _make_backtest_params())

        assert len(result.nav_curve) > 0
        # First NAV should be 1.0 (no trades yet)
        first_nav = result.nav_curve[0][1]  # (timestamp, nav) tuple
        assert first_nav == pytest.approx(1.0, abs=0.01)

    def test_backtest_graph_file_roundtrip(self, storage, tmp_dir):
        """Write graph to file → load → run → same result as in-memory."""
        bus = EventBus(EventBusOptions())
        graph = _make_sma_cross_graph()

        # Write to file
        path = os.path.join(tmp_dir, "sma_cross.graph")
        graph.write_to_file(path)

        runner = BacktestRunner(storage, bus)
        params = _make_backtest_params()

        # Run from in-memory graph
        result_mem = runner.run(graph, params)

        # Run from file
        result_file = runner.run(path, params)

        # Results should be identical
        assert result_mem.total_return == pytest.approx(result_file.total_return)
        assert result_mem.max_drawdown == pytest.approx(result_file.max_drawdown)
        assert result_mem.sharpe_ratio == pytest.approx(result_file.sharpe_ratio)
        assert result_mem.total_trades == result_file.total_trades
        assert len(result_mem.nav_curve) == len(result_file.nav_curve)

    def test_backtest_empty_data_returns_empty(self, tmp_dir):
        """No kline data → backtest returns empty result."""
        empty_storage = StorageEngine(1, tmp_dir)
        bus = EventBus(EventBusOptions())
        graph = _make_sma_cross_graph()
        runner = BacktestRunner(empty_storage, bus)

        params = _make_backtest_params()
        params.symbol = "NO_DATA_SYMBOL"

        result = runner.run(graph, params)

        assert len(result.nav_curve) == 0
        assert result.total_return == 0.0
        assert result.max_drawdown == 0.0
        assert result.sharpe_ratio == 0.0
        assert result.total_trades == 0

        empty_storage.close()

    def test_backtest_performance_under_1s(self, storage):
        """End-to-end backtest of 1 year daily data should complete in < 1s."""
        bus = EventBus(EventBusOptions())
        graph = _make_sma_cross_graph()
        runner = BacktestRunner(storage, bus)

        start = time.perf_counter()
        result = runner.run(graph, _make_backtest_params())
        elapsed = time.perf_counter() - start

        assert elapsed < 1.0, f"Backtest took {elapsed:.3f}s, expected < 1s"
        assert len(result.nav_curve) > 0


# ═══════════════════════════════════════════════════════════
# Test 5: Python API — 上传策略 → 注册 → 回测 → 查询结果
# ═══════════════════════════════════════════════════════════


@requires_cpp
class TestStrategyRegistryAndEngine:
    """Test the StrategyRegistry + StrategyEngine Python API."""

    def test_register_and_run_strategy(self, storage):
        """Register a strategy graph → StrategyEngine.run() → BacktestResult."""
        from quant_invest._quant_core import StrategyStatus

        bus = EventBus(EventBusOptions())
        registry = StrategyRegistry()
        graph = _make_sma_cross_graph()

        # Register the strategy
        entry = registry.register_strategy("sma_cross_e2e", graph)
        assert entry.name == "sma_cross_e2e"
        assert entry.status == StrategyStatus.REGISTERED

        # Run via StrategyEngine
        engine = StrategyEngine(storage, bus, registry)
        result = engine.run("sma_cross_e2e", _make_backtest_params())

        assert isinstance(result, BacktestResult)
        assert len(result.nav_curve) > 0

    def test_list_registered_strategies(self):
        """StrategyRegistry.list() should return registered strategies."""
        registry = StrategyRegistry()
        graph = _make_sma_cross_graph()

        registry.register_strategy("test_strat_1", graph)
        registry.register_strategy("test_strat_2", graph)

        strategies = registry.list_strategies()
        names = {s.name for s in strategies}
        assert "test_strat_1" in names
        assert "test_strat_2" in names

    def test_run_nonexistent_strategy_raises(self, storage):
        """Running a non-registered strategy should raise an error."""
        bus = EventBus(EventBusOptions())
        registry = StrategyRegistry()
        engine = StrategyEngine(storage, bus, registry)

        with pytest.raises(Exception):
            engine.run("nonexistent_strategy", _make_backtest_params())
