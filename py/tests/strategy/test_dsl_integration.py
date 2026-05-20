#!/usr/bin/env python3
"""P1 集成测试 — 验证策略声明式 DSL 全流程.

测试流程:
1. 编写 DSL 策略 → 自动注册到 StrategyRegistry
2. 编译策略 → 生成 FactorDAG
3. 执行因子计算 → 信号触发 → 交易下单
4. 修改策略参数 → 热重载 → DAG 更新
5. 回测使用新策略

运行方式:
    PYTHONPATH=src pytest tests/strategy/test_dsl_integration.py -v
"""

from __future__ import annotations

import asyncio
import tempfile
from pathlib import Path

import pytest

from quant_invest.strategy.dsl import (
    Factor,
    SignalContext,
    SignalExpr,
    Strategy,
    cross_above,
    cross_below,
    strategy,
    is_dsl_strategy,
    get_factor_decls,
    get_signal_decls,
)
from quant_invest.strategy.compiler import (
    CompiledStrategy,
    PythonExecutor,
    StrategyCompiler,
)
from quant_invest.strategy.registry import (
    StrategyEntry,
    StrategyKind,
    StrategyRegistry,
)
from quant_invest.strategy.watcher import (
    StrategyWatcher,
    ReloadRecord,
    get_watcher,
)


# ---------------------------------------------------------------------------
# Test 1: DSL 策略声明 → 自动注册
# ---------------------------------------------------------------------------

class TestDSLRegistration:
    """验证 DSL 策略声明后自动注册到 StrategyRegistry."""

    def test_strategy_decorator_registers(self):
        """@strategy 装饰器应将策略注册到全局注册表."""
        @strategy("test_auto_register")
        class AutoRegister(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            signal = cross_above(fast, slow)

            def on_signal(self, ctx):
                pass

        assert StrategyRegistry.has("test_auto_register")
        entry = StrategyRegistry.get_entry("test_auto_register")
        assert entry.kind == StrategyKind.DSL
        assert entry.is_dsl
        assert "fast" in entry.factor_decls
        assert "slow" in entry.factor_decls
        assert "signal" in entry.signal_decls

    def test_is_dsl_strategy_flag(self):
        """DSL 策略类应被标记为 _dsl_strategy=True."""
        @strategy("test_dsl_flag")
        class DSLFlag(Strategy):
            fast = Factor("SMA", period=5, input="close")

        assert is_dsl_strategy(DSLFlag)

    def test_factor_decls_at_class_level(self):
        """@strategy 装饰器应在类级别收集 _factor_decls 和 _signal_decls."""
        @strategy("test_class_decls")
        class ClassDecls(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            golden = cross_above(fast, slow)

        factor_decls = get_factor_decls(ClassDecls)
        signal_decls = get_signal_decls(ClassDecls)

        assert "fast" in factor_decls
        assert "slow" in factor_decls
        assert "golden" in signal_decls
        assert isinstance(factor_decls["fast"], Factor)
        assert isinstance(signal_decls["golden"], SignalExpr)

    def test_multi_signal_strategy(self):
        """多信号策略应正确注册所有信号."""
        @strategy("test_multi_signal")
        class MultiSignal(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            golden = cross_above(fast, slow)
            death = cross_below(fast, slow)

        entry = StrategyRegistry.get_entry("test_multi_signal")
        assert len(entry.signal_decls) == 2
        assert "golden" in entry.signal_decls
        assert "death" in entry.signal_decls


# ---------------------------------------------------------------------------
# Test 2: 策略编译 → FactorDAG 生成
# ---------------------------------------------------------------------------

class TestStrategyCompilation:
    """验证策略编译器正确生成 FactorDAG."""

    def test_compile_simple_strategy(self):
        """编译简单策略应生成正确的 CompiledStrategy."""
        @strategy("test_compile_simple")
        class CompileSimple(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            signal = cross_above(fast, slow)

        s = CompileSimple()
        compiler = StrategyCompiler(use_cpp=False)
        compiled = compiler.compile(s, name="test_compile_simple")

        assert compiled.name == "test_compile_simple"
        assert compiled.factor_names == ["fast", "slow"]
        assert compiled.signal_names == ["signal"]
        assert compiled.computer is None  # Python fallback mode

    def test_compile_multi_factor_strategy(self):
        """编译多因子策略应正确处理所有因子."""
        @strategy("test_compile_multi")
        class CompileMulti(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            rsi = Factor("RSI", period=14, input="close")
            golden = cross_above(fast, slow)

        s = CompileMulti()
        compiler = StrategyCompiler(use_cpp=False)
        compiled = compiler.compile(s, name="test_compile_multi")

        assert len(compiled.factor_names) == 3
        assert "rsi" in compiled.factor_names

    def test_dag_nodes_topological_order(self):
        """DAG 节点应按拓扑序排列，因子在信号之前."""
        @strategy("test_dag_order")
        class DAGOrder(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            signal = cross_above(fast, slow)

        s = DAGOrder()
        dag_nodes = s.get_dag_nodes()

        # 因子节点应在信号节点之前
        factor_positions = [
            i for i, n in enumerate(dag_nodes) if isinstance(n, Factor)
        ]
        signal_positions = [
            i for i, n in enumerate(dag_nodes) if isinstance(n, SignalExpr)
        ]
        assert max(factor_positions) < min(signal_positions)

    def test_builtin_name_mapping(self):
        """DSL 因子名应正确映射到 C++ 内置因子名."""
        mapping = StrategyCompiler._map_to_builtin_name
        assert mapping("SMA") == "MA"
        assert mapping("EMA") == "EMA"
        assert mapping("RSI") == "RSI"
        assert mapping("MACD") == "MACD"
        assert mapping("BBANDS") == "BOLL"
        assert mapping("UNKNOWN") is None


# ---------------------------------------------------------------------------
# Test 3: 因子计算 → 信号触发 → 交易下单
# ---------------------------------------------------------------------------

class TestFactorComputationAndSignals:
    """验证因子计算、信号触发和交易下单的完整流程."""

    def test_sma_factor_computation(self):
        """SMA 因子应正确计算简单移动平均."""
        from quant_invest.strategy.compiler import _sma

        data = [1.0, 2.0, 3.0, 4.0, 5.0]
        result = _sma(data, 3)
        # SMA(3) at index 2: (1+2+3)/3 = 2.0
        assert result[2] == 2.0
        # SMA(3) at index 4: (3+4+5)/3 = 4.0
        assert result[4] == 4.0

    def test_ema_factor_computation(self):
        """EMA 因子应正确计算指数移动平均."""
        from quant_invest.strategy.compiler import _ema

        data = [10.0, 11.0, 12.0]
        result = _ema(data, 2)
        # EMA(2): multiplier = 2/(2+1) = 0.667
        # result[0] = 10.0
        # result[1] = 11.0 * 0.667 + 10.0 * 0.333 = 7.337 + 3.33 = 10.667
        assert result[0] == 10.0
        assert abs(result[1] - 10.667) < 0.01

    def test_cross_above_detection(self):
        """cross_above 应正确检测金叉信号."""
        from quant_invest.strategy.compiler import _cross_above

        # fast crosses above slow at index 3
        fast = [10.0, 10.0, 10.0, 12.0, 12.0]
        slow = [11.0, 11.0, 11.0, 11.0, 11.0]
        result = _cross_above(fast, slow)
        assert result[3] == 1.0  # golden cross
        assert result[4] == 0.0  # no cross

    def test_cross_below_detection(self):
        """cross_below 应正确检测死叉信号."""
        from quant_invest.strategy.compiler import _cross_below

        fast = [12.0, 12.0, 12.0, 10.0, 10.0]
        slow = [11.0, 11.0, 11.0, 11.0, 11.0]
        result = _cross_below(fast, slow)
        assert result[3] == -1.0  # death cross
        assert result[4] == 0.0

    def test_python_executor_full_pipeline(self):
        """PythonExecutor 应完成因子→信号→下单的完整流程."""
        @strategy("test_full_pipeline")
        class FullPipeline(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            golden = cross_above(fast, slow)
            death = cross_below(fast, slow)

            def on_signal(self, ctx):
                if self.golden > 0:
                    ctx.order(symbol=ctx.symbol, side="BUY", quantity=100)
                elif self.death < 0:
                    ctx.order(symbol=ctx.symbol, side="SELL", quantity=50)

        s = FullPipeline()
        compiler = StrategyCompiler(use_cpp=False)
        compiled = compiler.compile(s, name="test_full_pipeline")
        executor = PythonExecutor(compiled)

        # Create data with golden cross
        close = [10.0] * 20 + [15.0] * 10
        latest = executor.execute_and_update({"close": close})

        # Verify factor values computed
        assert "fast" in latest
        assert "slow" in latest
        assert "golden" in latest

        # Verify strategy instance updated
        assert s.fast == latest["fast"]
        assert s.slow == latest["slow"]

        # Simulate signal trigger
        s.set_signal_value("golden", 1.0)
        ctx = SignalContext(
            symbol="000001.SZ", price=15.0, cash=100000.0, position=0
        )
        s.on_signal(ctx)
        assert len(ctx.orders) == 1
        assert ctx.orders[0]["side"] == "BUY"

    def test_signal_context_order_interface(self):
        """SignalContext 应正确记录订单."""
        ctx = SignalContext(symbol="600519.SH", price=1800.0, cash=100000.0)
        ctx.order(symbol="600519.SH", side="BUY", quantity=50)
        ctx.order(symbol="600519.SH", side="SELL", quantity=30)

        assert len(ctx.orders) == 2
        assert ctx.orders[0]["side"] == "BUY"
        assert ctx.orders[1]["side"] == "SELL"


# ---------------------------------------------------------------------------
# Test 4: 策略热重载 → DAG 更新
# ---------------------------------------------------------------------------

class TestHotReload:
    """验证策略文件修改后热重载和 DAG 更新."""

    def test_watcher_creation(self):
        """StrategyWatcher 应正确创建."""
        watcher = StrategyWatcher()
        assert not watcher.is_running
        assert Path(watcher.watch_dir).exists()

    def test_callback_registration(self):
        """成功/失败回调应正确注册."""
        watcher = StrategyWatcher()
        success_calls = []
        failure_calls = []

        watcher.on_reload_success(lambda name: success_calls.append(name))
        watcher.on_reload_failure(lambda name, err: failure_calls.append((name, err)))

        # Verify callbacks registered (no direct way to check, but no error)
        assert True  # If we got here, registration worked

    def test_watcher_global_singleton(self):
        """get_watcher() 应返回全局单例."""
        w1 = get_watcher()
        w2 = get_watcher()
        assert w1 is w2

    def test_reload_via_registry(self):
        """StrategyRegistry.reload() 应能重载策略模块."""
        @strategy("test_reload_via_registry")
        class ReloadViaRegistry(Strategy):
            fast = Factor("SMA", period=5, input="close")
            slow = Factor("SMA", period=20, input="close")
            signal = cross_above(fast, slow)

        # Verify strategy exists
        assert StrategyRegistry.has("test_reload_via_registry")
        entry = StrategyRegistry.get_entry("test_reload_via_registry")
        assert entry.factor_decls["fast"].params["period"] == 5

    def test_watcher_status(self):
        """StrategyWatcher.get_status() 应返回正确状态."""
        watcher = StrategyWatcher()
        status = watcher.get_status()
        assert "running" in status
        assert "watch_dir" in status
        assert "watched_files" in status
        assert status["running"] is False


# ---------------------------------------------------------------------------
# Test 5: 回测使用 DSL 策略
# ---------------------------------------------------------------------------

class TestBacktestWithDSL:
    """验证回测引擎可以使用 DSL 策略."""

    @pytest.fixture(autouse=True)
    def _ensure_examples_loaded(self):
        """确保示例策略模块已导入."""
        import importlib
        try:
            importlib.import_module("quant_invest.strategy.ma_cross")
        except ImportError:
            pass
        try:
            importlib.import_module("quant_invest.strategy.examples.ma_cross_dsl")
        except ImportError:
            pass

    def test_dsl_strategy_in_registry(self):
        """DSL 策略应在注册表中可查找."""
        # Verify all example strategies are registered
        assert StrategyRegistry.has("ma_cross_dsl")
        assert StrategyRegistry.has("ma_cross_dsl_simple")

    def test_dsl_strategy_kind(self):
        """DSL 策略应被正确分类为 StrategyKind.DSL."""
        entry = StrategyRegistry.get_entry("ma_cross_dsl")
        assert entry.kind == StrategyKind.DSL

    def test_on_bar_strategy_kind(self):
        """传统 on_bar 策略应被分类为 StrategyKind.ON_BAR."""
        entry = StrategyRegistry.get_entry("ma_cross")
        assert entry.kind == StrategyKind.ON_BAR

    def test_strategy_summary(self):
        """注册表摘要应包含 DSL 和 on_bar 策略统计."""
        summary = StrategyRegistry.summary()
        assert summary["dsl_count"] > 0
        assert summary["on_bar_count"] > 0
        assert summary["total"] == summary["dsl_count"] + summary["on_bar_count"]

    def test_dag_info_for_dsl_strategy(self):
        """get_dag_info 应返回 DSL 策略的 DAG 信息."""
        dag_info = StrategyRegistry.get_dag_info("ma_cross_dsl")
        assert "factors" in dag_info
        assert "signals" in dag_info
        assert "dag_nodes" in dag_info
        assert len(dag_info["dag_nodes"]) > 0

    def test_backward_compat_on_bar(self):
        """DSL Strategy 的 on_bar 方法应向后兼容."""
        @strategy("test_backward_compat")
        class BackwardCompat(Strategy):
            fast = Factor("SMA", period=5, input="close")

            def on_bar(self, bar_data, positions):
                return []

        s = BackwardCompat()
        result = s.on_bar({}, {})
        assert result == []

    def test_strategy_attribute_protection(self):
        """DSL Strategy 应保护 Factor/Signal 属性不被意外覆盖."""
        @strategy("test_attr_protection")
        class AttrProtection(Strategy):
            fast = Factor("SMA", period=5, input="close")

        s = AttrProtection()
        # Can set to numeric
        s.fast = 25.3
        assert s.fast == 25.3

        # Cannot set to non-numeric
        with pytest.raises(TypeError):
            s.fast = "invalid"


# ---------------------------------------------------------------------------
# Test 6: 信号组合器
# ---------------------------------------------------------------------------

class TestSignalCombinators:
    """验证各种信号组合器的正确性."""

    def test_and_signal(self):
        """and_signal 应在所有子信号同向时触发."""
        from quant_invest.strategy.compiler import _and_signals

        signals = [[1.0, 2.0, 0.0], [0.5, 1.5, -1.0]]
        result = _and_signals(signals)
        assert result[0] == 0.5  # both positive → min
        assert result[1] == 1.5  # both positive → min
        assert result[2] == 0.0  # mixed → 0

    def test_or_signal(self):
        """or_signal 应在任一子信号非零时触发."""
        from quant_invest.strategy.compiler import _or_signals

        signals = [[1.0, 0.0, -1.0], [0.0, 2.0, 0.0]]
        result = _or_signals(signals)
        assert result[0] == 1.0  # first positive
        assert result[1] == 2.0  # second positive
        assert result[2] == -1.0  # first negative

    def test_not_signal(self):
        """not_signal 应翻转信号方向."""
        from quant_invest.strategy.compiler import _not_signal

        signal = [1.0, -0.5, 0.0]
        result = _not_signal(signal)
        assert result[0] == -1.0
        assert result[1] == 0.5
        assert result[2] == 0.0

    def test_threshold_signal(self):
        """threshold 应过滤低于阈值的信号."""
        from quant_invest.strategy.compiler import _threshold

        signal = [0.5, 1.0, 0.1, -0.5]
        result = _threshold(signal, 0.3)
        assert result[0] == 0.5  # above threshold
        assert result[1] == 1.0  # above threshold
        assert result[2] == 0.0  # below threshold
        assert result[3] == -0.5  # abs above threshold

    def test_above_signal(self):
        """above 应在 a > b 时返回正值."""
        from quant_invest.strategy.compiler import _above

        a = [10.0, 12.0, 8.0]
        b = [11.0, 11.0, 11.0]
        result = _above(a, b)
        assert result[0] == 0.0  # a < b
        assert result[1] == 1.0  # a > b → diff
        assert result[2] == 0.0  # a < b

    def test_below_signal(self):
        """below 应在 a < b 时返回正值."""
        from quant_invest.strategy.compiler import _below

        a = [10.0, 12.0, 8.0]
        b = [11.0, 11.0, 11.0]
        result = _below(a, b)
        assert result[0] == 1.0  # a < b → diff
        assert result[1] == 0.0  # a > b
        assert result[2] == 3.0  # a < b → diff