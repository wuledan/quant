# P1-T1: Strategy Declarative DSL Design

## Overview

The Strategy Declarative DSL provides a Python-first, declarative way to define quantitative trading strategies. Instead of writing imperative `on_bar()` logic, strategies declare **factors** and **signals** as class attributes, and the system automatically compiles them into a C++ FactorDAG execution graph for high-performance computation.

**Key benefit**: Edit a `.py` strategy file, save, and it auto-reloads without restarting the backend.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Strategy .py file (declarative)                     │
│  ┌─────────────────────────────────────────────┐    │
│  │ @strategy("ma_cross")                       │    │
│  │ class MACross(Strategy):                    │    │
│  │     fast_ma = Factor("SMA", period=5, ...)  │    │
│  │     slow_ma = Factor("SMA", period=20, ...) │    │
│  │     signal = cross_above(fast_ma, slow_ma)  │    │
│  │     def on_signal(self, ctx): ...           │    │
│  └─────────────────────────────────────────────┘    │
└──────────────────────┬──────────────────────────────┘
                       │ @strategy decorator
                       ▼
┌─────────────────────────────────────────────────────┐
│  StrategyRegistry                                    │
│  - Registers strategy class                          │
│  - Records StrategyKind (DSL vs ON_BAR)             │
│  - Stores factor_decls, signal_decls metadata       │
│  - Provides get_dag_info() for compiler             │
└──────────────────────┬──────────────────────────────┘
                       │ StrategyCompiler.compile()
                       ▼
┌─────────────────────────────────────────────────────┐
│  C++ FactorDAG (via pybind11)                       │
│  - FactorRegistry: stores compute functions         │
│  - FactorDAG: builds dependency graph               │
│  - FactorComputer: executes in topological order    │
│  - BuiltInFactors: MA, EMA, RSI, MACD, BOLL        │
└─────────────────────────────────────────────────────┘
```

## DSL API Reference

### Factor

Declares a factor computation node that maps to a C++ FactorDAG operator.

```python
from quant_invest.strategy.dsl import Factor

fast_ma = Factor("SMA", period=5, input="close")
slow_ma = Factor("SMA", period=20, input="close")
rsi = Factor("RSI", period=14, input="close")
macd = Factor("MACD", fast=12, slow=26, signal=9, input="close")
```

**Parameters**:
- `kind` (str): Factor type name, maps to C++ BuiltInFactors operator
  - `"SMA"` / `"MA"` → C++ `MA` (Simple Moving Average)
  - `"EMA"` → C++ `EMA` (Exponential Moving Average)
  - `"RSI"` → C++ `RSI` (Relative Strength Index)
  - `"MACD"` → C++ `MACD`
  - `"BBANDS"` / `"BOLL"` → C++ `BOLL` (Bollinger Bands)
- `input` (str, default="close"): Input data column name
  - Common values: `"close"`, `"open"`, `"high"`, `"low"`, `"volume"`
- `**params`: Factor-specific parameters (e.g., `period=20`, `fast=12`)

**DAG behavior**: `Factor` is a leaf node with no dependencies. Its `dependencies()` returns `[]`.

### Signal Combinators

Combine factors and signals into trading signals.

#### cross_above(signal_a, signal_b) → SignalExpr

Golden cross: `signal_a` crosses above `signal_b` from below.

```
When signal_a[t-1] <= signal_b[t-1] AND signal_a[t] > signal_b[t]:
    Returns +1.0 (bullish signal)
Otherwise:
    Returns 0.0
```

#### cross_below(signal_a, signal_b) → SignalExpr

Death cross: `signal_a` crosses below `signal_b` from above.

```
When signal_a[t-1] >= signal_b[t-1] AND signal_a[t] < signal_b[t]:
    Returns -1.0 (bearish signal)
Otherwise:
    Returns 0.0
```

#### above(signal_a, signal_b) → SignalExpr

Persistent comparison: returns `signal_a - signal_b` when `a > b`, else `0.0`.

#### below(signal_a, signal_b) → SignalExpr

Persistent comparison: returns `signal_b - signal_a` when `a < b`, else `0.0`.

#### and_signal(*signals) → SignalExpr

Logical AND: all sub-signals must agree in direction. Returns the minimum absolute value when all same-direction, else `0.0`.

#### or_signal(*signals) → SignalExpr

Logical OR: any non-zero sub-signal triggers. Returns the strongest signal.

#### not_signal(signal) → SignalExpr

Signal negation: flips signal direction (`-value`).

#### threshold(signal, value) → SignalExpr

Threshold filter: passes signal through only when `abs(value) >= threshold`, else `0.0`.

### @strategy Decorator

Registers a DSL strategy class with the global `StrategyRegistry`.

```python
from quant_invest.strategy.dsl import strategy

@strategy("ma_cross")
class MACross(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    signal = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx):
        ...
```

**What the decorator does**:
1. Sets `cls._dsl_strategy = True` (marks as DSL strategy)
2. Collects `cls._factor_decls` — dict of `{attr_name: Factor}` instances
3. Collects `cls._signal_decls` — dict of `{attr_name: SignalExpr}` instances
4. Registers `cls` in `StrategyRegistry` with name, kind=DSL, and declaration metadata

### Strategy Base Class

```python
from quant_invest.strategy.dsl import Strategy

class Strategy(ABC):
    # Class attributes: Factor and SignalExpr declarations
    # Instance attributes: runtime float values (shadow class declarations)

    def on_signal(self, ctx: SignalContext) -> None:
        """Called when a signal fires. Override this."""

    def on_bar(self, bar_data, positions) -> list:
        """Traditional on_bar callback (backward compatible)."""

    def on_init(self) -> None:
        """Strategy initialization (optional)."""

    def on_finish(self) -> None:
        """Strategy cleanup (optional)."""
```

**Instance attribute shadowing**: On `__init__`, the Strategy creates instance-level `float` attributes that shadow the class-level `Factor`/`SignalExpr` declarations. This means `self.fast_ma` returns a `float` value at runtime, not the `Factor` object. The original declarations remain accessible via `self._factors` and `self._signals`.

**Attribute protection**: `__setattr__` prevents accidentally overwriting Factor/Signal names with non-numeric values. Only `int`/`float` assignments are allowed.

### SignalContext

Runtime context provided to `on_signal()` callback.

```python
from quant_invest.strategy.dsl import SignalContext

ctx = SignalContext(
    symbol="000001.SZ",
    price=10.0,
    cash=100000.0,
    position=0.0,
)
```

**Properties**:
- `symbol` (str): Current trading symbol
- `price` (float): Current price
- `cash` (float): Available cash
- `position` (float): Current position quantity
- `timestamp`: Current bar timestamp

**Methods**:
- `ctx.order(symbol="", side="BUY", quantity=0, price=0.0, order_type="MARKET")`: Place an order
  - `side`: `"BUY"` or `"SELL"`
  - `order_type`: `"MARKET"`, `"LIMIT"`, `"STOP"`
- `ctx.orders` (property): List of orders placed in this `on_signal()` call

## How Factors Declare Dependencies (for DAG Compilation)

Each `Factor` and `SignalExpr` is a `DAGNode` with:

- `node_id` (str): Unique identifier (auto-generated UUID hex[:12])
- `dependencies()` → `list[DAGNode]`: Returns upstream dependency nodes
  - `Factor.dependencies()` → `[]` (leaf node, input from raw market data)
  - `SignalExpr.dependencies()` → `list of operands` (e.g., `cross_above(fast, slow)` depends on `fast` and `slow`)
- `walk()` → `list[DAGNode]`: DFS topological traversal, returns nodes in dependency order (dependencies first)

**Example DAG structure**:

```python
fast_ma = Factor("SMA", period=5, input="close")
slow_ma = Factor("SMA", period=20, input="close")
signal = cross_above(fast_ma, slow_ma)

# DAG walk returns topological order:
# [fast_ma, slow_ma, signal]
# Factors come before signals that depend on them
```

**For the compiler (P1-T2)**: The compiler calls `strategy_instance.get_dag_nodes()` to get all nodes in topological order, then:
1. Register each `Factor` in C++ `FactorRegistry` with its compute function
2. Register each `SignalExpr` in C++ `FactorRegistry` with its signal compute function
3. Call `FactorDAG.build()` to construct the dependency graph
4. Create `FactorComputer` for execution

## DSL Strategies Coexisting with Traditional on_bar Strategies

The system supports both strategy types simultaneously:

### DSL Strategy (StrategyKind.DSL)

```python
@strategy("ma_cross_dsl")
class MACrossDSL(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    signal = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx):
        if self.signal > 0:
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=100)
```

### Traditional on_bar Strategy (StrategyKind.ON_BAR)

```python
@register("ma_cross")
class MACrossStrategy(StrategyBase):
    def on_bar(self, bar_data, positions):
        # Imperative logic
        signals = {}
        for symbol, df in bar_data.items():
            fast_ma = df["close"].rolling(5).mean().iloc[-1]
            slow_ma = df["close"].rolling(20).mean().iloc[-1]
            # ... crossing logic
        return signals
```

### Hybrid Strategy (DSL factors + on_bar logic)

```python
@strategy("ma_cross_hybrid")
class MACrossHybrid(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    # Declare factors but no signals — use on_bar for complex logic

    def on_bar(self, bar_data, positions):
        # Can access self.fast_ma, self.slow_ma (computed by engine)
        ...
```

### Registry API for Both Types

```python
from quant_invest.strategy.registry import StrategyRegistry, StrategyKind

# List by type
dsl_strategies = StrategyRegistry.list_dsl_strategies()
on_bar_strategies = StrategyRegistry.list_on_bar_strategies()

# Get entry with metadata
entry = StrategyRegistry.get_entry("ma_cross_dsl")
entry.kind        # StrategyKind.DSL
entry.is_dsl      # True
entry.factor_decls  # {"fast_ma": Factor(...), "slow_ma": Factor(...)}
entry.signal_decls  # {"signal": SignalExpr(...)}
entry.module_name   # "quant_invest.strategy.examples.ma_cross_dsl"

# Get DAG info (DSL only)
dag_info = StrategyRegistry.get_dag_info("ma_cross_dsl")
dag_info["factors"]    # {"fast_ma": Factor, "slow_ma": Factor}
dag_info["signals"]    # {"signal": SignalExpr}
dag_info["dag_nodes"]  # [Factor, Factor, SignalExpr] in topological order

# Summary
summary = StrategyRegistry.summary()
# {"total": 5, "dsl_count": 4, "on_bar_count": 1, "strategies": {...}}
```

## Example Strategies

### Simple MA Cross

```python
from quant_invest.strategy.dsl import Strategy, Factor, cross_above, strategy, SignalContext

@strategy("ma_cross_dsl")
class MACross(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    signal = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx: SignalContext):
        if self.signal > 0:
            # Golden cross: buy with 95% of cash
            buy_qty = ctx.cash * 0.95 / ctx.price
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=buy_qty)
        elif self.signal < 0:
            # Death cross: sell all
            ctx.order(symbol=ctx.symbol, side="SELL", quantity=ctx.position)
```

### Separate Golden/Death Cross Signals

```python
@strategy("ma_cross_full")
class MACrossFull(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    golden = cross_above(fast_ma, slow_ma)
    death = cross_below(fast_ma, slow_ma)

    def on_signal(self, ctx: SignalContext):
        if self.golden > 0:
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=ctx.cash * 0.95 / ctx.price)
        elif self.death < 0:
            ctx.order(symbol=ctx.symbol, side="SELL", quantity=ctx.position)
```

### Multi-Factor Strategy (RSI + MA)

```python
@strategy("rsi_ma_combo")
class RSIMACombo(Strategy):
    fast_ma = Factor("SMA", period=10, input="close")
    slow_ma = Factor("SMA", period=30, input="close")
    rsi = Factor("RSI", period=14, input="close")

    golden = cross_above(fast_ma, slow_ma)
    death = cross_below(fast_ma, slow_ma)

    def on_signal(self, ctx: SignalContext):
        if self.golden > 0:
            # Golden cross + RSI not overbought → buy
            ctx.order(symbol=ctx.symbol, side="BUY",
                      quantity=ctx.cash * 0.9 / ctx.price)
        elif self.death < 0:
            # Death cross → sell
            ctx.order(symbol=ctx.symbol, side="SELL",
                      quantity=ctx.position)
```

### Complex Signal Combination

```python
from quant_invest.strategy.dsl import and_signal, or_signal, threshold, above

@strategy("complex")
class ComplexStrategy(Strategy):
    fast = Factor("SMA", period=5, input="close")
    slow = Factor("SMA", period=20, input="close")
    rsi = Factor("RSI", period=14, input="close")

    ma_cross = cross_above(fast, slow)
    rsi_oversold = below(rsi, Factor("CONST", input="close", value=30))
    entry = and_signal(ma_cross, rsi_oversold)  # Both conditions must agree

    def on_signal(self, ctx: SignalContext):
        if self.entry > 0:
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=100)
```

## File Layout

```
py/src/quant_invest/strategy/
├── __init__.py          # Exports all DSL + legacy components
├── base.py              # Legacy StrategyBase (on_bar)
├── dsl.py               # DSL core: Factor, SignalExpr, Strategy, @strategy, SignalContext
├── compiler.py          # StrategyCompiler, CompiledStrategy, PythonExecutor
├── registry.py          # StrategyRegistry (refactored for DSL + on_bar)
├── watcher.py           # StrategyWatcher (file monitoring + hot reload)
├── context.py           # StrategyContext (legacy)
├── factor_api.py        # FactorAPI (C++ bridge)
├── signal.py            # SignalGenerator (legacy)
├── position_sizer.py    # PositionSizer
├── optimizer.py         # StrategyOptimizer
├── ma_cross.py          # Traditional on_bar MA cross strategy
└── examples/
    ├── __init__.py
    └── ma_cross_dsl.py  # DSL example strategies (MACross, MACrossSimple, etc.)
```

## Runtime Value Injection

The engine updates strategy instance values at each bar:

```python
# Engine calls these after computing factors/signals for each bar
strategy.set_factor_value("fast_ma", 25.3)
strategy.set_signal_value("golden", 1.0)

# In on_signal(), the strategy accesses values as floats:
def on_signal(self, ctx):
    if self.golden > 0:  # self.golden is 1.0 (float), not SignalExpr
        ctx.order(...)
    # self.fast_ma is 25.3 (float), not Factor
```

## Integration with Hot Reload (P1-T3)

The `StrategyWatcher` monitors the strategy directory for `.py` file changes:

1. File change detected → debounce (300ms)
2. Re-import the changed module via `importlib.reload()`
3. `@strategy` decorator re-fires → StrategyRegistry updated
4. New factor/signal declarations captured
5. Running strategy instances are NOT affected (they use their own compiled DAG)
6. New backtest runs will use the updated strategy

## Integration with Compiler (P1-T2)

The `StrategyCompiler` takes a DSL Strategy instance and produces a `CompiledStrategy`:

```python
from quant_invest.strategy.compiler import StrategyCompiler, PythonExecutor

compiler = StrategyCompiler(use_cpp=True)  # Falls back to Python if C++ unavailable
compiled = compiler.compile(strategy_instance, name="ma_cross")

# C++ mode: compiled.computer is a FactorComputer
result = compiled.compute(input_data={"close": [10.0, 11.0, ...]})

# Python fallback mode:
executor = PythonExecutor(compiled)
latest = executor.execute_and_update(input_data={"close": [10.0, 11.0, ...]})
# latest = {"fast_ma": 10.5, "slow_ma": 10.2, "signal": 1.0}
# strategy instance attributes also updated
```
