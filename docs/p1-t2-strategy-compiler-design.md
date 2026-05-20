# P1-T2: Strategy Compiler Design (Python AST → FactorDAG)

## 1. Overview

The strategy compiler translates DSL strategy declarations (Factor, SignalExpr) into executable computation graphs. It supports two modes:

- **C++ mode**: Registers factors in C++ FactorRegistry, builds FactorDAG, creates FactorComputer for high-performance execution
- **Python fallback**: Pure Python computation when `_quant_core` is unavailable

## 2. Compilation Pipeline

```
DSL Strategy Instance
    │
    ├─ Extract Factor declarations (Strategy.factors)
    ├─ Extract SignalExpr declarations (Strategy.signals)
    │
    ├─ For each Factor:
    │   ├─ Generate compute function (_make_factor_compute_fn)
    │   ├─ Register in C++ FactorRegistry (or Python fallback)
    │   └─ Map node_id → registered_name
    │
    ├─ For each SignalExpr:
    │   ├─ Generate compute function (_make_signal_compute_fn)
    │   ├─ Register in C++ FactorRegistry
    │   └─ Map node_id → registered_name
    │
    ├─ Build FactorDAG (C++ mode)
    ├─ Validate DAG
    ├─ Create FactorComputer (C++ mode)
    │
    └─ Return CompiledStrategy
```

## 3. Key Classes

### StrategyCompiler

- `compile(strategy_instance, name) → CompiledStrategy`: Main entry point
- `_compile_cpp(...)`: C++ FactorRegistry + FactorDAG + FactorComputer
- `_compile_python(...)`: Pure Python fallback, no C++ objects
- `_register_factor_cpp(...)`: Register Factor in C++ FactorRegistry
- `_register_signal_cpp(...)`: Register SignalExpr in C++ FactorRegistry

### CompiledStrategy

- `compute(input_data) → dict`: Execute via C++ FactorComputer
- `compute_factor(name, input_data) → dict`: Single factor computation
- Properties: name, strategy, factor_names, signal_names, computer, registry, dag

### PythonExecutor

- `execute(input_data) → dict`: Pure Python execution, topological order
- `execute_and_update(input_data) → dict`: Execute + update Strategy instance values

## 4. Factor Compute Functions

Each Factor generates a compute function that:
- Checks if C++ BuiltInFactors has a matching operator (SMA→MA, EMA→EMA, RSI→RSI)
- Falls back to pure Python implementations: `_sma()`, `_ema()`, `_rsi()`

### Built-in Factor Mapping

| DSL Kind | C++ BuiltIn Name | Python Fallback |
|----------|-----------------|-----------------|
| SMA / MA | MA | `_sma()` |
| EMA | EMA | `_ema()` |
| RSI | RSI | `_rsi()` |
| MACD | MACD | (not implemented) |
| BBANDS / BOLL | BOLL | (not implemented) |

## 5. Signal Compute Functions

Signal expressions use combinator functions:

| Operator | Function | Description |
|----------|----------|-------------|
| cross_above | `_cross_above()` | Golden cross: a crosses above b → +1.0 |
| cross_below | `_cross_below()` | Death cross: a crosses below b → -1.0 |
| above | `_above()` | a > b → positive difference |
| below | `_below()` | a < b → positive difference |
| and | `_and_signals()` | All signals same direction → min absolute |
| or | `_or_signals()` | Any signal triggers → max absolute |
| not | `_not_signal()` | Negate signal |
| threshold | `_threshold()` | Filter by absolute value |

## 6. Testing

Verified with Python fallback mode:
- Factor computation: SMA(5), SMA(20) produce correct moving averages
- Signal computation: cross_above/cross_below detect crossing points
- Strategy instance update: `set_factor_value()` and `set_signal_value()` work correctly

## 7. Limitations (Current)

- C++ FactorComputer not yet fully integrated (FactorComputer class not in pybind11 bindings)
- MACD, BOLL Python fallback not implemented
- No caching of compiled strategies across sessions