#!/usr/bin/env python3
"""test_pybind_import.py — Verify _quant_core pybind11 module import and basic functionality.

Run from project root:
    source py/.venv/bin/activate
    PYTHONPATH=py/src/quant_invest python cpp/test/test_pybind_import.py
"""
import sys
import traceback

# Ensure _quant_core can be found
_imported_via_package = False
_module = None

try:
    import quant_invest._quant_core as _module
    _imported_via_package = True
except ImportError:
    try:
        import _quant_core as _module
    except ImportError as e:
        print(f"FAIL: Cannot import _quant_core: {e}")
        print("Make sure the module is built and PYTHONPATH is set correctly.")
        sys.exit(1)

print(f"OK: _quant_core imported via {'quant_invest._quant_core' if _imported_via_package else '_quant_core'}")
print(f"    Version: {_module.__version__}")

# ── Track test results ──
passed = 0
failed = 0

def test(name, fn):
    global passed, failed
    try:
        fn()
        print(f"  PASS: {name}")
        passed += 1
    except Exception as e:
        print(f"  FAIL: {name}: {e}")
        traceback.print_exc()
        failed += 1


# ════════════════════════════════════════════════════════════════════
# 1. Module-level checks
# ════════════════════════════════════════════════════════════════════

def test_version():
    assert _module.__version__ == "0.1.0", f"Expected version 0.1.0, got {_module.__version__}"

def test_enums_exist():
    # DataField enum (C++ has no TIMESTAMP; starts at OPEN=0)
    assert hasattr(_module, 'DataField')
    assert _module.DataField.OPEN.value == 0
    assert _module.DataField.CLOSE.value == 3
    assert _module.DataField.VOLUME.value == 4

    # StoreStatus enum
    assert hasattr(_module, 'StoreStatus')
    assert _module.StoreStatus.OK.value == 0

    # Codec enum
    assert hasattr(_module, 'Codec')
    assert _module.Codec.NONE.value == 0
    assert _module.Codec.DELTA.value == 1
    assert _module.Codec.GORILLA.value == 4

    # DataType enum
    assert hasattr(_module, 'DataType')

    # OrderSide enum
    assert hasattr(_module, 'OrderSide')
    assert _module.OrderSide.BUY.value == 0
    assert _module.OrderSide.SELL.value == 1

    # OrderType enum
    assert hasattr(_module, 'OrderType')
    assert _module.OrderType.MARKET.value == 0
    assert _module.OrderType.LIMIT.value == 1

    # OrderStatus enum
    assert hasattr(_module, 'OrderStatus')

    # TimeInForce enum
    assert hasattr(_module, 'TimeInForce')

    # ConnectionStatus enum
    assert hasattr(_module, 'ConnectionStatus')

    # AlertSeverity enum
    assert hasattr(_module, 'AlertSeverity')

test("Module version", test_version)
test("Enums exist", test_enums_exist)

# ════════════════════════════════════════════════════════════════════
# 2. StorageEngine
# ════════════════════════════════════════════════════════════════════

def test_storage_engine_create():
    opts = _module.StorageEngineOptions()
    engine = _module.StorageEngine(opts)
    assert engine is not None

def test_storage_engine_custom_opts():
    opts = _module.StorageEngineOptions(256, '/tmp/quant_test_pybind')
    engine = _module.StorageEngine(opts)
    assert engine is not None

def test_storage_engine_flush():
    opts = _module.StorageEngineOptions(256, '/tmp/quant_test_pybind')
    engine = _module.StorageEngine(opts)
    result = engine.flush()
    assert result == _module.StoreStatus.OK

def test_storage_engine_close():
    opts = _module.StorageEngineOptions(256, '/tmp/quant_test_pybind')
    engine = _module.StorageEngine(opts)
    result = engine.close()
    assert result == _module.StoreStatus.OK

def test_column_block_compress_decompress():
    import numpy as np
    data = np.array([100.0, 101.5, 102.3, 103.8, 104.0], dtype=np.float64)
    block = _module.ColumnBlock.compress_double(
        _module.DataField.CLOSE, data, _module.Codec.GORILLA, 0, 4)
    assert block.row_count == 5
    assert block.compressed_size > 0
    decompressed = block.decompress_double()
    np.testing.assert_array_almost_equal(decompressed, data)

def test_column_block_delta_codec():
    import numpy as np
    timestamps = np.array([1000000, 1000001, 1000002, 1000003, 1000004], dtype=np.int64)
    block = _module.ColumnBlock.compress_int64(
        _module.DataField.VOLUME, timestamps, _module.Codec.DELTA, 1000000, 1000004)
    assert block.row_count == 5
    decompressed = block.decompress_int64()
    np.testing.assert_array_equal(decompressed, timestamps)

def test_kline_row():
    row = _module.KlineRow()
    row.timestamp = 1700000000000
    row.open_price = 10000
    row.close_price = 10500
    assert row.timestamp == 1700000000000
    assert row.open_price == 10000

def test_time_range():
    tr = _module.TimeRange(1000, 2000)
    assert tr.begin_ts == 1000
    assert tr.end_ts == 2000

test("StorageEngine create (default opts)", test_storage_engine_create)
test("StorageEngine create (custom opts)", test_storage_engine_custom_opts)
test("StorageEngine flush", test_storage_engine_flush)
test("StorageEngine close", test_storage_engine_close)
test("ColumnBlock compress/decompress (Gorilla)", test_column_block_compress_decompress)
test("ColumnBlock compress/decompress (Delta)", test_column_block_delta_codec)
test("KlineRow", test_kline_row)
test("TimeRange", test_time_range)

# ════════════════════════════════════════════════════════════════════
# 3. FactorDAG + FactorRegistry
# ════════════════════════════════════════════════════════════════════

def test_factor_registry():
    registry = _module.FactorRegistry()
    assert registry.size() == 0

def test_factor_meta():
    meta = _module.FactorMeta()
    meta.name = "test_ma"
    meta.description = "Test MA factor"
    meta.version = 1
    assert meta.name == "test_ma"
    assert meta.description == "Test MA factor"
    assert meta.version == 1

def test_factor_dag():
    registry = _module.FactorRegistry()
    dag = _module.FactorDAG(registry)
    assert not dag.is_built

def test_factor_dag_validate():
    registry = _module.FactorRegistry()
    dag = _module.FactorDAG(registry)
    result = dag.validate()
    assert result.valid

def test_builtin_factors():
    assert hasattr(_module.BuiltInFactors, 'register_all')
    assert hasattr(_module.BuiltInFactors, 'register_ma')
    assert hasattr(_module.BuiltInFactors, 'ma')

def test_builtin_ma():
    import numpy as np
    values = np.array([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0], dtype=np.float64)
    result = _module.BuiltInFactors.ma(values, 3)
    assert len(result) == 7

test("FactorRegistry create", test_factor_registry)
test("FactorMeta", test_factor_meta)
test("FactorDAG create", test_factor_dag)
test("FactorDAG validate", test_factor_dag_validate)
test("BuiltInFactors accessible", test_builtin_factors)
test("BuiltInFactors.ma()", test_builtin_ma)

# ════════════════════════════════════════════════════════════════════
# 4. OrderManager + Execution
# ════════════════════════════════════════════════════════════════════

def test_order_manager_create():
    om = _module.OrderManager()
    assert om.total_order_count == 0
    assert om.active_order_count == 0

def test_order_manager_create_order():
    om = _module.OrderManager()
    req = _module.OrderRequest()
    req.symbol = "000001"
    req.side = _module.OrderSide.BUY
    req.type = _module.OrderType.LIMIT
    req.tif = _module.TimeInForce.DAY
    req.price = 10000
    req.quantity = 100
    order_id = om.create_order(req)
    assert order_id is not None
    assert om.total_order_count == 1

def test_order_state_machine():
    assert _module.OrderStateMachine.is_valid_transition(
        _module.OrderStatus.PENDING_NEW, _module.OrderStatus.NEW)
    assert _module.OrderStateMachine.is_terminal(_module.OrderStatus.FILLED)
    assert _module.OrderStateMachine.is_active(_module.OrderStatus.NEW)

def test_mock_broker():
    broker = _module.MockBroker()
    assert broker.status == _module.ConnectionStatus.DISCONNECTED
    broker.connect()
    assert broker.status == _module.ConnectionStatus.CONNECTED

def test_order_struct():
    order = _module.Order()
    order.symbol = "600000"
    order.side = _module.OrderSide.BUY
    order.quantity = 1000
    assert order.symbol == "600000"
    assert order.quantity == 1000

test("OrderManager create", test_order_manager_create)
test("OrderManager create_order", test_order_manager_create_order)
test("OrderStateMachine transitions", test_order_state_machine)
test("MockBroker", test_mock_broker)
test("Order struct", test_order_struct)

# ════════════════════════════════════════════════════════════════════
# 5. RiskEngine
# ════════════════════════════════════════════════════════════════════

def test_risk_engine_create():
    engine = _module.RiskEngine()
    assert engine.is_enabled

def test_risk_engine_with_config():
    cfg = _module.CircuitBreakerConfig()
    cfg.drawdown_threshold = 0.15
    engine = _module.RiskEngine(cfg)
    assert engine.is_enabled

def test_risk_rules():
    dd_rule = _module.MaxDrawdownRule(0.10)
    assert dd_rule.max_drawdown_pct == 0.10

    conc_rule = _module.ConcentrationRule(0.30)
    assert conc_rule.max_concentration_pct == 0.30

    exp_rule = _module.ExposureRule(0.80)
    assert exp_rule.max_exposure_ratio == 0.80

    limit_rule = _module.LimitRule(100000, 500000)
    assert limit_rule.max_order_value == 100000
    assert limit_rule.max_total_value == 500000

def test_risk_check():
    engine = _module.RiskEngine()
    ctx = _module.RiskContext()
    ctx.total_equity = 1000000
    ctx.available_cash = 500000
    result = engine.check(ctx)
    assert result.approved

def test_risk_alert():
    alert = _module.RiskAlert()
    alert.rule_id = 1
    alert.rule_name = "MaxDrawdown"
    alert.severity = _module.AlertSeverity.WARNING
    assert alert.rule_id == 1

test("RiskEngine create", test_risk_engine_create)
test("RiskEngine with config", test_risk_engine_with_config)
test("Risk rules", test_risk_rules)
test("Risk check (pass)", test_risk_check)
test("RiskAlert", test_risk_alert)

# ════════════════════════════════════════════════════════════════════
# 6. Shared Memory (zero-copy utilities)
# ════════════════════════════════════════════════════════════════════

def test_vector_to_numpy():
    import numpy as np
    v = [1.0, 2.0, 3.0, 4.0, 5.0]
    arr = _module.vector_to_numpy(v)
    assert len(arr) == 5
    np.testing.assert_array_equal(arr, v)

def test_numpy_to_vector():
    import numpy as np
    arr = np.array([10.0, 20.0, 30.0], dtype=np.float64)
    v = _module.numpy_to_vector(arr)
    assert len(v) == 3
    assert v == [10.0, 20.0, 30.0]

def test_shm_allocate():
    _module.shm.allocate('test_pybind_seg', 2048)
    info = _module.shm.get('test_pybind_seg')
    assert info is not None
    assert info['name'] == 'test_pybind_seg'
    assert info['size'] == 2048

def test_shm_write_read():
    import numpy as np
    _module.shm.allocate('test_pybind_rw', 1024)
    data = np.array([1.5, 2.5, 3.5], dtype=np.float64)
    n = _module.shm.write_doubles('test_pybind_rw', data)
    assert n == 3
    result = _module.shm.read_doubles('test_pybind_rw', 3)
    np.testing.assert_array_almost_equal(result, data)
    _module.shm.release('test_pybind_rw')

test("vector_to_numpy", test_vector_to_numpy)
test("numpy_to_vector", test_numpy_to_vector)
test("shm allocate", test_shm_allocate)
test("shm write/read doubles", test_shm_write_read)

# ════════════════════════════════════════════════════════════════════
# Summary
# ════════════════════════════════════════════════════════════════════

print()
print("=" * 60)
print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
print("=" * 60)

if failed > 0:
    sys.exit(1)
else:
    print("All tests passed!")
    sys.exit(0)