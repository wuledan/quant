# P2-T2: 因子计算引擎桥接设计文档

## 概述

FactorEngineBridge 封装 C++ FactorDAG 计算引擎，为回测引擎提供每根 K 线的因子计算。当 C++ FactorComputer 因 pybind11 限制无法直接从 Python 构造时，使用 C++ BuiltInFactors 静态方法进行高性能因子计算，Python 侧计算信号组合器。

## 架构

```
┌─────────────────────────────────────────────────────┐
│  回测引擎 (BacktestEngine)                           │
│  ┌─────────────────────────────────────────────┐    │
│  │ DSL 策略: _tick_dsl()                       │    │
│  │   1. FactorEngineBridge.on_bar()            │    │
│  │   2. strategy.on_signal(ctx)                │    │
│  │   3. 组合管理 → 订单执行                     │    │
│  │                                             │    │
│  │ 传统策略: _tick() (原有逻辑)                 │    │
│  └─────────────────────────────────────────────┘    │
└──────────────────────┬──────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  FactorEngineBridge (因子引擎桥接)                    │
│  ┌─────────────────────────────────────────────┐    │
│  │ 1. 收集历史 K 线数据（滑动窗口）             │    │
│  │ 2. C++ BuiltInFactors.ma/ema/rsi → 因子值   │    │
│  │ 3. Python 信号组合器 → 信号值               │    │
│  │ 4. strategy.set_factor_value() /             │    │
│  │    strategy.set_signal_value()               │    │
│  └─────────────────────────────────────────────┘    │
└──────────────────────┬──────────────────────────────┘
                       │ C++ 因子计算
                       ▼
┌─────────────────────────────────────────────────────┐
│  C++ BuiltInFactors (通过 pybind11)                   │
│  - ma(data, period)   → numpy.ndarray               │
│  - ema(data, period)  → numpy.ndarray               │
│  - rsi(data, period)  → numpy.ndarray               │
│  - macd(data, f, s, sig) → (macd, signal, hist)     │
│  - boll(data, period, mult) → (upper, mid, lower)   │
└─────────────────────────────────────────────────────┘
```

## FactorEngineBridge API

### 创建与初始化

```python
from quant_invest.strategy.factor_engine import FactorEngineBridge

bridge = FactorEngineBridge(
    strategy=strategy_instance,  # DSL Strategy 实例
    use_cpp=True,               # 是否使用 C++ 因子引擎（默认 True）
    history_bars=100,           # 历史数据窗口大小（默认 100）
)
bridge.initialize()             # 初始化（构建拓扑序、验证 DAG）
print(bridge.cpp_available)     # C++ 引擎是否可用
```

### 每根 K 线调用

```python
# 在回测引擎的 _tick_dsl() 中调用
result = bridge.on_bar(
    symbol="000001.SZ",         # 标的代码
    bar_data={                  # K 线数据
        "close": 10.5,
        "open": 10.3,
        "high": 10.8,
        "low": 10.1,
        "volume": 50000,
    },
)
# result: FactorComputeResult(
#   factor_values={"fast_ma": 25.3, "slow_ma": 20.1},
#   signal_values={"golden": 1.0, "death": 0.0},
#   compute_time_ns=15000,
#   cpp_used=True,
# )
```

### FactorComputeResult 数据结构

```python
@dataclass
class FactorComputeResult:
    factor_values: dict[str, float] = field(default_factory=dict)
    signal_values: dict[str, float] = field(default_factory=dict)
    compute_time_ns: int = 0       # 计算耗时（纳秒）
    cpp_used: bool = False         # 是否使用了 C++ 引擎
```

### 获取因子/信号序列

```python
# 获取因子的完整计算序列
fast_ma_series = bridge.get_factor_series("fast_ma")
# [nan, nan, nan, nan, 10.2, 10.5, 10.8, ...]

# 获取信号的完整计算序列
golden_series = bridge.get_signal_series("golden")
# [0.0, 0.0, 1.0, 0.0, 0.0, ...]
```

### 重置引擎

```python
bridge.reset()  # 清空历史数据和缓存（新回测开始时调用）
```

## C++ 因子计算映射

FactorEngineBridge 内部维护 DSL Factor 类型到 C++ BuiltInFactors 方法的映射:

| DSL Factor.kind | C++ BuiltInFactors 方法 | 说明 |
|---|---|---|
| `"SMA"` / `"MA"` | `BuiltInFactors.ma(data, period)` | 简单移动平均 |
| `"EMA"` | `BuiltInFactors.ema(data, period)` | 指数移动平均 |
| `"RSI"` | `BuiltInFactors.rsi(data, period)` | 相对强弱指标 |
| `"MACD"` | `BuiltInFactors.macd(data, fast, slow, signal)` | MACD 指标 |
| `"BBANDS"` / `"BOLL"` | `BuiltInFactors.boll(data, period, mult)` | 布林带 |

**数据格式**: C++ BuiltInFactors 接受 `numpy.ndarray[float64]` 输入，返回 `numpy.ndarray[float64]` 输出。

## 信号组合器计算

信号组合器（cross_above, cross_below 等）始终在 Python 中计算，因为 C++ 不提供信号组合器 API:

| 信号操作 | 计算函数 | 说明 |
|---|---|---|
| `cross_above` | `_compute_cross_above(a, b)` | 金叉: a 上穿 b → 1.0 |
| `cross_below` | `_compute_cross_below(a, b)` | 死叉: a 下穿 b → -1.0 |
| `above` | `_compute_above(a, b)` | a > b 时返回差值 |
| `below` | `_compute_below(a, b)` | a < b 时返回差值绝对值 |
| `and` | `_compute_and(signals)` | 信号与: 同向取最小 |
| `or` | `_compute_or(signals)` | 信号或: 任一非零触发 |
| `not` | `_compute_not(signal)` | 信号取反 |
| `threshold` | `_compute_threshold(signal, value)` | 阈值过滤 |

## FactorComputer 限制与解决方案

### 问题

C++ `FactorComputer` 构造函数接受 `unique_ptr<FactorRegistry>` 和 `unique_ptr<FactorDAG>` 参数，pybind11 无法自动将 Python 对象转换为 `unique_ptr`，导致无法从 Python 直接构造 `FactorComputer`。

### 解决方案

1. **因子计算**: 使用 C++ `BuiltInFactors` 静态方法（`ma`、`ema`、`rsi` 等）进行高性能单因子计算
2. **DAG 验证**: 使用 `FactorRegistry` + `FactorDAG` 进行依赖关系验证和拓扑排序
3. **信号计算**: 在 Python 中计算信号组合器（C++ 不提供信号组合器 API）
4. **纯 Python 回退**: 当 C++ 不可用时，使用纯 Python 实现（`_sma`、`_ema`、`_rsi`）

## 回测引擎集成

BacktestEngine 新增 `use_factor_engine` 参数:

```python
engine = BacktestEngine(
    data_handler=data_handler,
    broker=broker,
    portfolio=portfolio,
    strategy=strategy,
    initial_cash=1_000_000.0,
    use_factor_engine=True,       # 启用 FactorEngineBridge（默认 True）
    use_cpp_execution=True,       # 启用 C++ OrderManager 执行
)
```

**集成逻辑**:

1. `use_factor_engine=True` 时，引擎在 `__init__` 中检测策略是否为 DSL 策略
2. 如果是 DSL 策略，初始化 `FactorEngineBridge`
3. 在 `_tick()` 中，根据策略类型选择执行路径:
   - DSL 策略: `_tick_dsl()` → `FactorEngineBridge.on_bar()` + `strategy.on_signal()`
   - 传统策略: 原有 `_tick()` 逻辑
4. `_tick_dsl()` 中:
   - 对每个标的调用 `FactorEngineBridge.on_bar()` 计算因子和信号
   - 调用 `strategy.on_signal(ctx)` 生成订单
   - 将 DSL 订单转为 `SignalEvent` → `OrderEvent`
   - 执行订单（C++ 或 Python）

**向后兼容**: `use_factor_engine` 默认为 `True`，但仅对 DSL 策略生效。传统策略不受影响。

## 纯 Python 回退模式

当 C++ `_quant_core` 模块不可用时，FactorEngineBridge 使用纯 Python 模拟:

- `_sma()`: 简单移动平均
- `_ema()`: 指数移动平均
- `_rsi()`: 相对强弱指标
- 信号组合器: 始终在 Python 中计算

## 历史数据管理

FactorEngineBridge 维护一个滑动窗口缓存:

- 每根 K 线追加到 `_history[symbol][column]` 列表
- 超过 `history_bars`（默认 100）时截断
- 因子计算使用完整历史数据（如 MA(20) 需要至少 20 根 K 线）
- 不足数据时返回 `NaN`

## 文件布局

```
py/src/quant_invest/strategy/
├── factor_engine.py         # FactorEngineBridge 核心实现
├── dsl.py                   # DSL 核心（Factor, SignalExpr, Strategy）
├── compiler.py              # StrategyCompiler（P1-T2）
└── ...

py/src/quant_invest/backtest/
├── engine.py                # 新增 use_factor_engine 参数和 _tick_dsl()
└── ...
```

## 使用示例

### 基本用法

```python
from quant_invest.strategy.factor_engine import FactorEngineBridge
from quant_invest.strategy.dsl import Factor, cross_above, Strategy, strategy

@strategy("ma_cross")
class MACross(Strategy):
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    golden = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx):
        if self.golden > 0:
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=100)

# 创建桥接
strategy_instance = MACross()
bridge = FactorEngineBridge(strategy_instance)
bridge.initialize()

# 模拟 K 线
for price in [10.0, 10.5, 11.0, ...]:
    result = bridge.on_bar("000001.SZ", {"close": price})
    print(f"fast_ma={strategy_instance.fast_ma:.2f}, golden={strategy_instance.golden}")
```

### 在回测引擎中使用

```python
from quant_invest.backtest.engine import BacktestEngine

# DSL 策略自动启用 FactorEngineBridge
engine = BacktestEngine(
    data_handler=data_handler,
    broker=broker,
    portfolio=portfolio,
    strategy=macross_strategy,  # DSL Strategy 实例
    use_factor_engine=True,
)

result = engine.run(start_date, end_date)
```
