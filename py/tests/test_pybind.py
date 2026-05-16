"""Tests for C++ pybind11 bindings."""

from __future__ import annotations

import math
import random
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest


pytest.importorskip("quant_invest._quant_core")

from quant_invest.c_bindings import (
    AlertSeverity,
    AlgoOrderConfig,
    AlgoTraderStats,
    BrokerConfig,
    BuiltInFactors,
    CircuitBreakerConfig,
    Codec,
    ColumnBlock,
    ComputeResult,
    ConcentrationRule,
    ConnectionStatus,
    DAGValidationResult,
    DataField,
    DataType,
    ExposureRule,
    FactorComputer,
    FactorDAG,
    FactorMeta,
    FactorRegistry,
    FillReport,
    IRiskRule,
    KlineRow,
    LimitRule,
    MaxDrawdownRule,
    MockBroker,
    Order,
    OrderManager,
    OrderRequest,
    OrderSide,
    OrderStateMachine,
    OrderStatus,
    OrderType,
    RiskAlert,
    RiskCheckResult,
    RiskCheckResultSet,
    RiskContext,
    RiskEngine,
    RiskEngineStats,
    RuleId,
    StorageEngine,
    StorageEngineOptions,
    StoreStatus,
    TimeInForce,
    TimeRange,
    TimeSeriesStore,
    numpy_to_vector,
    shm,
    vector_to_numpy,
)


# ═══════════════════════════════════════════
# Storage Engine Tests
# ═══════════════════════════════════════════


class TestDataField:
    def test_enum_values(self):
        assert DataField.OPEN.value == 0
        assert DataField.CLOSE.value == 3
        assert DataField.VOLUME.value == 4

    def test_enum_names(self):
        assert DataField.OPEN.name == "OPEN"
        assert DataField.VWAP.name == "VWAP"


class TestDataType:
    def test_enum_values(self):
        assert DataType.KLINE_1MIN.value == 0
        assert DataType.KLINE_DAY.value == 5


class TestColumnBlock:
    def test_compress_decompress_double(self):
        data = np.array([10.5, 20.3, 30.1, 40.7, 50.2], dtype=np.float64)
        block = ColumnBlock.compress_double(
            DataField.CLOSE, data, Codec.GORILLA, 1000, 5000
        )
        assert block.row_count == 5
        assert block.field == DataField.CLOSE
        assert block.codec == Codec.GORILLA
        assert block.min_timestamp == 1000
        assert block.max_timestamp == 5000
        assert block.compressed_size > 0

        decoded = block.decompress_double()
        np.testing.assert_array_almost_equal(decoded, data)

    def test_compress_decompress_int64(self):
        data = np.array([100, 200, 300, 400, 500], dtype=np.int64)
        block = ColumnBlock.compress_int64(
            DataField.VOLUME, data, Codec.DELTA, 0, 0
        )
        assert block.row_count == 5
        assert block.field == DataField.VOLUME

        decoded = block.decompress_int64()
        np.testing.assert_array_equal(decoded, data)

    def test_to_dict(self):
        data = np.array([1.0, 2.0], dtype=np.float64)
        block = ColumnBlock.compress_double(DataField.OPEN, data)
        d = block.to_dict()
        assert d["field"] == 0
        assert d["row_count"] == 2
        assert d["compressed_size"] > 0


class TestTimeSeriesStore:
    def test_put_and_query(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            store = TimeSeriesStore(64, tmpdir)
            data = np.array([10.0, 20.0, 30.0], dtype=np.float64)
            block = ColumnBlock.compress_double(DataField.CLOSE, data)

            status = store.put("000001.SZ", 0, block)
            assert status == StoreStatus.OK

            results = store.query(
                "000001.SZ", 0, DataField.CLOSE, TimeRange(0, 9999999999)
            )
            assert len(results) == 1
            decoded = results[0].decompress_double()
            np.testing.assert_array_almost_equal(decoded, data)

            status = store.flush()
            assert status == StoreStatus.OK
            status = store.close()
            assert status == StoreStatus.OK


class TestStorageEngine:
    def test_store_and_query_kline(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            opts = StorageEngineOptions(64, tmpdir)
            engine = StorageEngine(opts)

            row = KlineRow()
            row.timestamp = 1000000
            row.open_price = 1000
            row.high_price = 1100
            row.low_price = 900
            row.close_price = 1050
            row.volume = 100000
            row.amount = 100000000
            row.vwap = 1040

            status = engine.store_kline("600000.SS", DataType.KLINE_1MIN, row)
            assert status == StoreStatus.OK

            result = engine.query_kline(
                "600000.SS",
                DataType.KLINE_1MIN,
                DataField.CLOSE,
                TimeRange(0, 9999999999),
            )
            assert len(result.timestamps) > 0
            assert len(result.values) > 0

            engine.close()


# ═══════════════════════════════════════════
# Factor Engine Tests
# ═══════════════════════════════════════════


class TestFactorRegistry:
    def test_register_and_lookup(self):
        reg = FactorRegistry()
        meta = FactorMeta()
        meta.name = "test_factor"
        meta.description = "A test factor"
        meta.inputs = ["close", "volume"]
        meta.outputs = ["result"]

        def compute_fn(inputs: dict[str, list[float]]) -> dict[str, list[float]]:
            close = inputs.get("close", [])
            return {"result": [2.0 * v for v in close]}

        fid = reg.register_factor(meta, compute_fn)
        assert fid == 1

        meta2 = reg.get_meta("test_factor")
        assert meta2 is not None
        assert meta2.name == "test_factor"

        assert reg.has_factor("test_factor")
        assert reg.size() == 1

    def test_list_factors(self):
        reg = FactorRegistry()
        BuiltInFactors.register_all(reg)
        factors = reg.list_factors()
        assert len(factors) >= 5  # MA, EMA, RSI, MACD, Boll


class TestFactorComputer:
    def test_compute_all(self):
        reg = FactorRegistry()
        dag = FactorDAG(reg)
        BuiltInFactors.register_all(reg)
        dag.build()

        computer = FactorComputer.__new__(FactorComputer)
        # FactorComputer needs unique_ptr args, so we re-register manually
        reg2 = FactorRegistry()
        dag2 = FactorDAG(reg2)

        def ma_fn(inputs: dict[str, list[float]]) -> dict[str, list[float]]:
            values = inputs.get("close", [])
            if len(values) < 2:
                return {"ma": values[:]}
            result = [0.0] * len(values)
            for i in range(len(values)):
                window = values[max(0, i - 4) : i + 1]
                result[i] = sum(window) / len(window)
            return {"ma": result}

        meta = FactorMeta()
        meta.name = "custom_ma"
        meta.inputs = ["close"]
        meta.outputs = ["ma"]
        reg2.register_factor(meta, ma_fn)
        dag2.build()

        computer2 = FactorComputer(reg2, dag2)
        input_data = {
            "close": [10.0, 11.0, 12.0, 13.0, 14.0, 15.0],
        }
        result = computer2.compute_all(input_data)
        assert result.success
        assert "ma" in result.outputs
        assert len(result.outputs["ma"]) == 6


class TestBuiltInFactors:
    def test_ma(self):
        values = np.array([1.0, 2.0, 3.0, 4.0, 5.0], dtype=np.float64)
        result = BuiltInFactors.ma(values, 3)
        assert len(result) == 5
        # MA(3): [NaN, NaN, 2, 3, 4] — but implemented as valid values
        assert result[2] == pytest.approx(2.0)

    def test_ema(self):
        values = np.array([1.0, 2.0, 3.0, 4.0, 5.0], dtype=np.float64)
        result = BuiltInFactors.ema(values, 3)
        assert len(result) == 5

    def test_rsi(self):
        values = np.array(
            [45.0, 46.0, 47.0, 48.0, 49.0, 50.0, 49.0, 48.0], dtype=np.float64
        )
        result = BuiltInFactors.rsi(values, 3)
        assert len(result) == 8


class TestFactorDAG:
    def test_build_and_validate(self):
        reg = FactorRegistry()
        dag = FactorDAG(reg)
        BuiltInFactors.register_all(reg)
        dag.build()
        assert dag.is_built

        validation = dag.validate()
        assert validation.valid

        topo = dag.topological_sort()
        assert len(topo) > 0

        levels = dag.parallel_levels()
        assert len(levels) > 0


# ═══════════════════════════════════════════
# Execution Engine Tests
# ═══════════════════════════════════════════


class TestOrderEnums:
    def test_order_side(self):
        assert OrderSide.BUY.value == 0
        assert OrderSide.SELL.value == 1

    def test_order_type(self):
        assert OrderType.MARKET.value == 0
        assert OrderType.LIMIT.value == 1

    def test_order_status(self):
        assert OrderStatus.PENDING_NEW.value == 0
        assert OrderStatus.FILLED.value == 3
        assert OrderStatus.REJECTED.value == 6


class TestOrder:
    def test_create_order(self):
        order = Order()
        order.order_id = 1
        order.symbol = "000001.SZ"
        order.side = OrderSide.BUY
        order.type = OrderType.LIMIT
        order.quantity = 1000
        order.price = 5000

        assert order.order_id == 1
        assert order.symbol == "000001.SZ"
        assert order.side == OrderSide.BUY

        rep = repr(order)
        assert "000001.SZ" in rep
        assert "Buy" in rep


class TestOrderStateMachine:
    def test_valid_transitions(self):
        assert OrderStateMachine.is_valid_transition(
            OrderStatus.PENDING_NEW, OrderStatus.NEW
        )
        assert OrderStateMachine.is_valid_transition(
            OrderStatus.NEW, OrderStatus.FILLED
        )
        assert not OrderStateMachine.is_valid_transition(
            OrderStatus.FILLED, OrderStatus.NEW
        )

    def test_state_checks(self):
        assert OrderStateMachine.is_terminal(OrderStatus.FILLED)
        assert OrderStateMachine.is_terminal(OrderStatus.CANCELLED)
        assert not OrderStateMachine.is_terminal(OrderStatus.NEW)
        assert OrderStateMachine.is_active(OrderStatus.NEW)
        assert OrderStateMachine.can_cancel(OrderStatus.NEW)
        assert not OrderStateMachine.can_cancel(OrderStatus.FILLED)


class TestOrderManager:
    def test_create_and_lifecycle(self):
        om = OrderManager()
        req = OrderRequest()
        req.symbol = "000001.SZ"
        req.side = OrderSide.BUY
        req.type = OrderType.LIMIT
        req.price = 5000
        req.quantity = 1000

        result = om.create_order(req)
        assert result.ok
        order_id = result.value
        assert order_id > 0

        order = om.find_order(order_id)
        assert order is not None
        assert order.symbol == "000001.SZ"
        assert order.status == OrderStatus.PENDING_NEW
        assert om.total_order_count == 1

    def test_order_accept_and_fill(self):
        om = OrderManager()
        req = OrderRequest()
        req.symbol = "600000.SS"
        req.quantity = 500
        result = om.create_order(req)
        oid = result.value

        om.on_order_accepted(oid, "brk-001")
        order = om.find_order(oid)
        assert order is not None
        assert order.status == OrderStatus.NEW

        om.on_order_fill(oid, 200, 5100)
        order = om.find_order(oid)
        assert order is not None
        assert order.status == OrderStatus.PARTIAL_FILLED
        assert order.filled_quantity == 200

        om.on_order_fill(oid, 300, 5100)
        order = om.find_order(oid)
        assert order.status == OrderStatus.FILLED

    def test_order_rejection(self):
        om = OrderManager()
        req = OrderRequest()
        req.symbol = "000001.SZ"
        result = om.create_order(req)
        oid = result.value

        om.on_order_rejected(oid, "insufficient margin")
        order = om.find_order(oid)
        assert order.status == OrderStatus.REJECTED
        assert order.reject_reason == "insufficient margin"

    def test_find_by_symbol(self):
        om = OrderManager()
        for sym in ["A", "A", "B"]:
            req = OrderRequest()
            req.symbol = sym
            om.create_order(req)

        a_orders = om.find_orders_by_symbol("A")
        assert len(a_orders) == 2

    def test_active_order_count(self):
        om = OrderManager()
        req = OrderRequest()
        req.symbol = "X"
        oid = om.create_order(req).value
        assert om.active_order_count >= 1

        om.on_order_fill(oid, 100, 5000)
        assert om.active_order_count == 0


class TestMockBroker:
    def test_connect_and_authenticate(self):
        config = BrokerConfig()
        config.broker_id = "test_broker"
        broker = MockBroker(config)

        assert broker.status == ConnectionStatus.DISCONNECTED
        broker.connect()
        assert broker.status == ConnectionStatus.CONNECTED

        result = broker.authenticate("my_token")
        assert result

    def test_submit_and_cancel(self):
        broker = MockBroker()
        broker.connect()
        broker.authenticate("token")

        order = Order()
        order.order_id = 1
        order.symbol = "000001.SZ"
        broker.submit_order(order)

        assert len(broker.submitted) == 1
        broker.cancel_order(1)
        assert len(broker.cancelled) == 1


# ═══════════════════════════════════════════
# Risk Engine Tests
# ═══════════════════════════════════════════


class TestRiskCheckResult:
    def test_pass(self):
        r = RiskCheckResult.pass_result()
        assert r.approved

    def test_reject(self):
        r = RiskCheckResult.reject_result(
            1, "MaxDrawdown", "DD exceeded", 0.15, 0.10
        )
        assert not r.approved
        assert r.rule_id == 1
        assert r.rule_name == "MaxDrawdown"
        assert r.exposure == 0.15
        assert r.limit == 0.10


class TestBuiltInRiskRules:
    def test_max_drawdown_rule(self):
        rule = MaxDrawdownRule(0.10)
        ctx = RiskContext()
        ctx.max_drawdown = 0.05
        result = rule.check(ctx)
        assert result.approved

        ctx.max_drawdown = 0.15
        result = rule.check(ctx)
        assert not result.approved

    def test_concentration_rule(self):
        rule = ConcentrationRule(0.20)
        ctx = RiskContext()
        ctx.total_equity = 1000000
        ctx.symbol_positions = {"000001.SZ": 150000}
        result = rule.check(ctx)
        assert result.approved

        ctx.symbol_positions["000001.SZ"] = 300000
        result = rule.check(ctx)
        assert not result.approved

    def test_exposure_rule(self):
        rule = ExposureRule(0.80)
        ctx = RiskContext()
        ctx.total_equity = 1000000
        ctx.total_positions = 700000
        result = rule.check(ctx)
        assert result.approved

        ctx.total_positions = 900000
        result = rule.check(ctx)
        assert not result.approved

    def test_limit_rule(self):
        rule = LimitRule(50000, 500000)
        ctx = RiskContext()
        ctx.order_side = 1
        ctx.order_price = 50.0
        ctx.order_quantity = 100  # order value = 5000
        result = rule.check(ctx)
        assert result.approved

        ctx.order_quantity = 2000  # order value = 100000
        result = rule.check(ctx)
        assert not result.approved


class TestRiskEngine:
    def test_register_rules_and_check(self):
        engine = RiskEngine()
        engine.register_rule(MaxDrawdownRule(0.10))
        engine.register_rule(ConcentrationRule(0.20))
        engine.register_rule(ExposureRule(0.80))
        engine.register_rule(LimitRule(50000, 500000))

        assert engine.is_enabled
        assert len(engine.all_rules) == 4

        ctx = RiskContext()
        ctx.total_equity = 1000000
        ctx.total_positions = 500000
        ctx.max_drawdown = 0.05

        result = engine.check(ctx)
        assert result.approved
        assert len(result.rule_results) == 4

    def test_rule_rejection(self):
        engine = RiskEngine()
        engine.register_rule(MaxDrawdownRule(0.10))

        ctx = RiskContext()
        ctx.total_equity = 1000000
        ctx.max_drawdown = 0.20

        result = engine.check(ctx)
        assert not result.approved
        assert len(result.rule_results) == 1
        assert not result.rule_results[0].approved

    def test_enable_disable(self):
        engine = RiskEngine()
        assert engine.is_enabled
        engine.disable()
        assert not engine.is_enabled
        engine.enable()
        assert engine.is_enabled

    def test_circuit_breaker(self):
        config = CircuitBreakerConfig()
        config.max_consecutive_rejects = 2
        engine = RiskEngine(config)
        engine.register_rule(MaxDrawdownRule(0.05))

        ctx = RiskContext()
        ctx.max_drawdown = 0.10

        # First reject
        engine.check(ctx)
        assert not engine.is_circuit_break

        # Second consecutive reject should trigger circuit breaker
        engine.check(ctx)
        assert engine.is_circuit_break

        engine.reset_circuit_break()
        assert not engine.is_circuit_break

    def test_stats(self):
        engine = RiskEngine()
        engine.register_rule(MaxDrawdownRule(0.10))

        ctx = RiskContext()
        stats = engine.stats
        assert stats.total_checks == 0

        engine.check(ctx)
        stats = engine.stats
        assert stats.total_checks == 1


class TestAlertSeverity:
    def test_severity_values(self):
        assert AlertSeverity.INFO.value == 0
        assert AlertSeverity.WARNING.value == 1
        assert AlertSeverity.ERROR.value == 2
        assert AlertSeverity.CRITICAL.value == 3


class TestRiskAlert:
    def test_create_alert(self):
        alert = RiskAlert()
        alert.rule_id = 1
        alert.rule_name = "MaxDrawdown"
        alert.severity = AlertSeverity.WARNING
        alert.message = "DD exceeded"
        alert.value = 0.15
        alert.threshold = 0.10

        assert alert.rule_id == 1
        assert alert.severity == AlertSeverity.WARNING


# ═══════════════════════════════════════════
# Zero-Copy Transfer Tests
# ═══════════════════════════════════════════


class TestVectorNumpyConversion:
    def test_vector_to_numpy(self):
        # We can't easily call vector_to_numpy from Python since it takes
        # a C++ std::vector<double>&, but we test the numpy bindings via ColumnBlock
        pass


class TestSharedMemory:
    def test_allocate_and_read_write(self):
        info = shm.allocate("test_data", 1024)
        assert info["name"] == "test_data"
        assert info["size"] == 1024

        data = np.array([1.0, 2.0, 3.0, 4.0, 5.0], dtype=np.float64)
        n = shm.write_doubles("test_data", data)
        assert n == 5

        result = shm.read_doubles("test_data", 5)
        np.testing.assert_array_almost_equal(result, data)

    def test_release(self):
        shm.allocate("temp_data", 64)
        shm.release("temp_data")
        info = shm.get("temp_data")
        assert info is None


# ═══════════════════════════════════════════
# Integration Tests
# ═══════════════════════════════════════════


class TestStorageRiskIntegration:
    """Integration: store kline data, then check risk against it."""

    def test_kline_to_risk_check(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            engine = StorageEngine(StorageEngineOptions(64, tmpdir))
            risk = RiskEngine()
            risk.register_rule(ExposureRule(0.80))

            # Store some data
            for i in range(5):
                row = KlineRow()
                row.timestamp = 1000000 + i * 60000000
                row.open_price = 1000 + i * 10
                row.high_price = 1100 + i * 10
                row.low_price = 900 + i * 10
                row.close_price = 1050 + i * 10
                row.volume = 100000
                row.amount = 100000000
                row.vwap = 1040 + i * 10
                engine.store_kline("600000.SS", DataType.KLINE_1MIN, row)
            engine.flush()

            # Query and verify
            result = engine.query_kline(
                "600000.SS",
                DataType.KLINE_1MIN,
                DataField.CLOSE,
                TimeRange(0, 9999999999),
            )
            assert len(result.values) == 5
            assert result.values[-1] > result.values[0]

            # Risk check
            ctx = RiskContext()
            ctx.total_equity = 1000000
            ctx.total_positions = 600000
            check = risk.check(ctx)
            assert check.approved

            engine.close()


class TestExecutionRiskIntegration:
    """Integration: execute order with risk check."""

    def test_order_with_risk_approval(self):
        om = OrderManager()
        risk = RiskEngine()
        risk.register_rule(LimitRule(100000, 1000000))

        # Create order
        req = OrderRequest()
        req.symbol = "000001.SZ"
        req.side = OrderSide.BUY
        req.type = OrderType.LIMIT
        req.price = 5000
        req.quantity = 10  # order value = 50000 < 100000 limit

        result = om.create_order(req)
        oid = result.value

        # Risk check before accepting
        ctx = RiskContext()
        ctx.total_equity = 1000000
        ctx.order_symbol = "000001.SZ"
        ctx.order_quantity = 10
        ctx.order_price = 50.0
        ctx.order_side = 1

        check = risk.check(ctx)
        assert check.approved

        om.on_order_accepted(oid, "brk-001")
        om.on_order_fill(oid, 10, 5000)

        order = om.find_order(oid)
        assert order.status == OrderStatus.FILLED


# ═══════════════════════════════════════════
# Module Metadata Tests
# ═══════════════════════════════════════════


class TestModuleMetadata:
    def test_module_importable(self):
        import quant_invest._quant_core as qc

        assert hasattr(qc, "__version__")
        assert hasattr(qc, "__doc__")

    def test_submodules_exist(self):
        import quant_invest._quant_core as qc

        assert hasattr(qc, "shm")
        assert hasattr(qc, "vector_to_numpy")
        assert hasattr(qc, "numpy_to_vector")


class TestAlgoConfig:
    def test_algo_config(self):
        cfg = AlgoOrderConfig()
        cfg.side = OrderSide.BUY
        cfg.symbol = "000001.SZ"
        cfg.total_quantity = 10000
        assert cfg.side == OrderSide.BUY
        assert cfg.symbol == "000001.SZ"


class TestBrokerConfig:
    def test_broker_config_defaults(self):
        cfg = BrokerConfig()
        assert cfg.timeout_ms == 5000
        assert cfg.auto_reconnect
        assert cfg.reconnect_interval_ms == 3000
