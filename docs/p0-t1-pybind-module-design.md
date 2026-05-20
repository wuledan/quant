# P0-T1: pybind11 Module Build & Verification Design

## 1. Build System Design

### CMakeLists.txt Structure

The project uses a single top-level `CMakeLists.txt` at `/home/wuledan/work/proj/quant_invest/CMakeLists.txt` that builds both C++ test binaries and the pybind11 module.

**Key build configuration:**

```cmake
cmake_minimum_required(VERSION 3.20)
project(quant_invest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pybind11 REQUIRED)

# pybind11 module: _quant_core
pybind11_add_module(_quant_core
    cpp/quant/pybind/pybind_module.cc
    cpp/quant/pybind/py_storage.cc
    cpp/quant/pybind/py_factor.cc
    cpp/quant/pybind/py_execution.cc
    cpp/quant/pybind/py_risk.cc
    # ... all C++ implementation files
)

target_include_directories(_quant_core PRIVATE
    ${CMAKE_SOURCE_DIR}/cpp
    ${CMAKE_SOURCE_DIR}/cpp/third_party
)

target_link_libraries(_quant_core PRIVATE
    fmt::fmt
    folly
)

# Install the .so into the Python package
install(TARGETS _quant_core
    LIBRARY DESTINATION py/src/quant_invest
)
```

### Dependencies

| Dependency | Purpose | Notes |
|---|---|---|
| pybind11 | Python/C++ bindings | v2.13.6, installed in project venv |
| fmt | String formatting | Used by logging/error handling |
| folly | Facebook's C++ library | Used for `fbstring`, `F14FastMap`, coroutines |
| liburing | io_uring support | Used by StorageEngine async I/O |
| pthreads | Threading | Used by SharedMemory, EventBus |

### Compiler Flags

- `-std=c++20` — Required for coroutines, concepts, ranges
- `-fPIC` — Required for shared library
- `-O2` — Release optimization level
- pybind11 automatically adds: `-fvisibility=hidden`, `-g`, Python include dirs

### Build Commands

```bash
# From project root
source py/.venv/bin/activate

# Configure (one-time or after CMakeLists.txt changes)
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")

# Build
cmake --build build -j$(nproc)

# Or build just the module
cmake --build build --target _quant_core -j$(nproc)
```

The built `.so` file is placed at `py/src/quant_invest/_quant_core.cpython-312-x86_64-linux-gnu.so`.

---

## 2. pybind11 Module API Surface

The `_quant_core` module exposes the following classes, enums, and functions:

### Enums

| Enum | Values | Description |
|---|---|---|
| `DataField` | TIMESTAMP(0), OPEN(1), HIGH(2), LOW(3), VOLUME(4), CLOSE(5) | Column identifiers for time-series data |
| `StoreStatus` | OK(0), NOT_FOUND(1), CORRUPTED(2), IO_ERROR(3) | Storage operation status codes |
| `Codec` | RAW(0), GORILLA(1), DELTA(2), ZIGZAG(3) | Compression codec types |
| `DataType` | INT64(0), DOUBLE(1), STRING(2) | Data type enumeration |
| `OrderSide` | BUY(0), SELL(1) | Order direction |
| `OrderType` | LIMIT(0), MARKET(1) | Order type |
| `OrderStatus` | PENDING_NEW(0), NEW(1), PARTIALLY_FILLED(2), FILLED(3), CANCELLED(4), REJECTED(5) | Order lifecycle states |
| `TimeInForce` | DAY(0), GTC(1), IOC(2), FOK(3) | Order time-in-force |
| `ConnectionStatus` | DISCONNECTED(0), CONNECTING(1), CONNECTED(2), RECONNECTING(3) | Broker connection state |
| `AlertSeverity` | INFO(0), WARNING(1), CRITICAL(2) | Risk alert severity levels |

### Storage Classes

| Class | Key Methods/Properties | Description |
|---|---|---|
| `StorageEngineOptions` | `shard_count: int`, `data_dir: str` | Configuration for StorageEngine |
| `StorageEngine` | `flush()`, `close()`, `upsert_kline()`, `query_kline()` | Columnar time-series storage with Gorilla/Delta compression |
| `ColumnBlock` | `compress_double()`, `compress_int64()`, `decompress_double()`, `decompress_int64()`, `row_count`, `compressed_size` | Compressed column block with zero-copy numpy interop |
| `KlineRow` | `timestamp`, `open_price`, `high_price`, `low_price`, `close_price`, `volume`, `amount`, `turnover` | Single K-line row (A-share format) |
| `TimeRange` | `begin_ts`, `end_ts` | Time range query parameter |

### Factor Classes

| Class | Key Methods/Properties | Description |
|---|---|---|
| `FactorRegistry` | `size()`, `register_factor()`, `get()` | Global factor metadata registry |
| `FactorMeta` | `name`, `description`, `inputs`, `outputs`, `version` | Factor descriptor |
| `FactorDAG` | `is_built`, `validate()`, `build()`, `compute()` | Directed acyclic graph for factor computation ordering |
| `BuiltInFactors` | `register_all()`, `register_ma()`, `ma()` | Static built-in factor implementations |

### Execution Classes

| Class | Key Methods/Properties | Description |
|---|---|---|
| `OrderManager` | `create_order()`, `cancel_order()`, `total_order_count`, `active_order_count` | Order lifecycle management with state machine |
| `OrderRequest` | `symbol`, `side`, `type`, `tif`, `price`, `quantity` | New order request |
| `Order` | `order_id`, `symbol`, `side`, `status`, `filled_qty` | Order object |
| `OrderStateMachine` | `is_valid_transition()`, `is_terminal()`, `is_active()` | Static state transition validator |
| `MockBroker` | `connect()`, `disconnect()`, `submit()`, `status` | Simulated broker for backtesting |
| `AlgorithmicTrader` | `submit()`, `cancel()` | TWAP/VWAP algorithmic execution |

### Risk Classes

| Class | Key Methods/Properties | Description |
|---|---|---|
| `RiskEngine` | `check()`, `is_enabled`, `enable()`, `disable()` | Pre-trade risk check engine |
| `CircuitBreakerConfig` | `drawdown_threshold`, `max_order_rate` | Circuit breaker configuration |
| `MaxDrawdownRule` | `max_drawdown_pct` | Maximum drawdown risk rule |
| `ConcentrationRule` | `max_concentration_pct` | Position concentration limit |
| `ExposureRule` | `max_exposure_ratio` | Net exposure limit |
| `LimitRule` | `max_order_value`, `max_total_value` | Order/position size limits |
| `RiskContext` | `total_equity`, `available_cash`, `positions` | Current portfolio state for risk check |
| `RiskCheckResult` | `approved`, `violations` | Risk check outcome |
| `RiskAlert` | `rule_id`, `rule_name`, `severity`, `message` | Risk alert notification |

### Utility Functions

| Function | Signature | Description |
|---|---|---|
| `vector_to_numpy` | `(list[float]) -> ndarray` | Convert C++ vector to numpy array (zero-copy) |
| `numpy_to_vector` | `(ndarray) -> list[float]` | Convert numpy array to C++ vector |
| `shm.allocate` | `(name, size) -> None` | Allocate shared memory segment |
| `shm.get` | `(name) -> dict` | Get shared memory segment info |
| `shm.release` | `(name) -> None` | Release shared memory segment |
| `shm.write_doubles` | `(name, ndarray) -> int` | Write doubles to shared memory |
| `shm.read_doubles` | `(name, count) -> ndarray` | Read doubles from shared memory |

---

## 3. Issues Encountered and Resolutions

### Issue 1: pybind11 not found by CMake
**Problem**: Initial CMake configuration failed because pybind11 was not installed in the project virtual environment.
**Resolution**: `pip install pybind11` in the project venv, then pass `-Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")` to cmake.

### Issue 2: Module import path
**Problem**: `_quant_core.so` is built into `build/`, but Python needs it in `py/src/quant_invest/` for proper package import.
**Resolution**: The CMakeLists.txt already has an install step that copies the .so to `py/src/quant_invest/`. After building, the module can be imported as `import quant_invest._quant_core` or `import _quant_core` (with PYTHONPATH).

### Issue 3: No compilation errors
**Surprise**: The C++ code compiled cleanly on the first try with no errors. The existing CMakeLists.txt was well-configured and all pybind11 bindings were correctly written.

---

## 4. How to Build and Verify

### Prerequisites
```bash
# Activate the project virtual environment
source py/.venv/bin/activate

# Ensure pybind11 is installed
pip install pybind11
```

### Build
```bash
# Configure (first time or after CMakeLists.txt changes)
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -Dpybind11_DIR=$(python3 -c "import pybind11; print(pybind11.get_cmake_dir())")

# Build the module
cmake --build build --target _quant_core -j$(nproc)
```

### Verify
```bash
# Quick smoke test
PYTHONPATH=py/src/quant_invest python3 -c "
import _quant_core
print('Version:', _quant_core.__version__)

# Test key classes
engine = _quant_core.StorageEngine(_quant_core.StorageEngineOptions())
dag = _quant_core.FactorDAG(_quant_core.FactorRegistry())
om = _quant_core.OrderManager()
risk = _quant_core.RiskEngine()
print('All key classes instantiated OK')
"

# Full test suite
PYTHONPATH=py/src/quant_invest python3 cpp/test/test_pybind_import.py
```

### Test Results (2026-05-20)
All 30 tests pass:
- 2 module-level checks (version, enums)
- 8 storage tests (StorageEngine, ColumnBlock, KlineRow, TimeRange)
- 6 factor tests (FactorRegistry, FactorMeta, FactorDAG, BuiltInFactors)
- 5 execution tests (OrderManager, OrderStateMachine, MockBroker, Order)
- 5 risk tests (RiskEngine, rules, check, alert)
- 4 shared memory tests (vector/numpy conversion, shm allocate/read/write)
