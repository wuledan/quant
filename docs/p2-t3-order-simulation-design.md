# P2-T3: 订单模拟设计文档

## 概述

订单模拟模块封装 C++ OrderManager 和 MockBroker，为回测引擎提供统一的订单模拟接口。当 C++ 执行引擎不可用时，自动回退到纯 Python 模拟，确保向后兼容。

## 架构

```
┌─────────────────────────────────────────────────────┐
│  回测引擎 (BacktestEngine)                           │
│  ┌─────────────────────────────────────────────┐    │
│  │ use_cpp_execution=True:                     │    │
│  │   OrderAdapter → C++ OrderManager           │    │
│  │                → C++ MockBroker             │    │
│  │                                             │    │
│  │ use_cpp_execution=False (默认):             │    │
│  │   SimulatedBroker (Python)                  │    │
│  └─────────────────────────────────────────────┘    │
└──────────────────────┬──────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  OrderAdapter (订单适配器)                           │
│  ┌─────────────────────────────────────────────┐    │
│  │ submit_order()  → C++ OrderManager 或 Python│    │
│  │ cancel_order()  → C++ OrderManager 或 Python│    │
│  │ get_order()     → C++ Order 或 Python dict  │    │
│  │ get_active_orders() → 活跃订单列表           │    │
│  │ simulate_fill() → 模拟成交                  │    │
│  │ connect_broker() / disconnect_broker()      │    │
│  └─────────────────────────────────────────────┘    │
└──────────────────────┬──────────────────────────────┘
                       │ C++ 模式
                       ▼
┌─────────────────────────────────────────────────────┐
│  C++ 执行引擎 (通过 pybind11)                        │
│  - OrderManager: 订单生命周期管理                     │
│  - MockBroker: 模拟经纪商（需先 connect）             │
│  - OrderRequest / Order: 订单数据结构                │
│  - OrderSide / OrderType / OrderStatus: 枚举         │
│  - TimeInForce / ConnectionStatus: 枚举              │
└─────────────────────────────────────────────────────┘
```

## OrderAdapter API

### 创建与连接

```python
from quant_invest.execution.order_adapter import OrderAdapter

adapter = OrderAdapter()           # 自动检测 C++ 可用性
adapter.connect_broker()           # 连接 MockBroker（提交订单前必须调用）
print(adapter.cpp_available)       # C++ 引擎是否可用
print(adapter.is_connected)        # MockBroker 是否已连接
```

### 提交订单

```python
result = adapter.submit_order(
    symbol="000001.SZ",           # 标的代码
    side="BUY",                   # 买卖方向: "BUY" / "SELL"
    order_type="MARKET",          # 订单类型: "MARKET" / "LIMIT" / "STOP"
    price=10.0,                   # 订单价格（MARKET 单可设 0）
    quantity=1000,                # 订单数量
    stop_price=0.0,              # 止损价（STOP 单使用）
    time_in_force="DAY",         # 有效期: "DAY" / "IOC" / "GTC"
)
# result: OrderResult(order_id=1, status="FILLED", filled_qty=1000, fill_price=10.0)
```

### OrderResult 数据结构

```python
@dataclass
class OrderResult:
    order_id: int | None = None   # 订单 ID（None 表示提交失败）
    status: str = ""              # 订单状态
    filled_qty: int = 0           # 已成交数量
    fill_price: float = 0.0       # 成交均价
    reject_reason: str = ""       # 拒绝原因

    @property
    def success(self) -> bool:    # 订单是否成功提交
        ...
```

### 查询订单

```python
# 查询单个订单
order_info = adapter.get_order(order_id=1)
# order_info: OrderInfo(order_id=1, symbol="000001.SZ", side="BUY",
#                       status="FILLED", filled_quantity=1000, ...)

# 获取所有活跃订单
active_orders = adapter.get_active_orders()
# 返回状态为 PENDING_NEW / NEW / PARTIAL_FILLED 的订单列表
```

### OrderInfo 数据结构

```python
@dataclass
class OrderInfo:
    order_id: int = 0
    symbol: str = ""
    side: str = ""                # "BUY" / "SELL"
    order_type: str = ""          # "MARKET" / "LIMIT" / "STOP"
    status: str = ""              # 订单状态字符串
    price: float = 0.0
    quantity: int = 0
    filled_quantity: int = 0
    avg_fill_price: float = 0.0
    reject_reason: str = ""
```

### 取消订单

```python
success = adapter.cancel_order(order_id=2)
# 返回 True/False
```

### 模拟成交（回测引擎调用）

```python
# 在每根 bar 后，回测引擎调用 simulate_fill 更新订单状态
result = adapter.simulate_fill(
    order_id=1,
    fill_price=10.5,
    fill_quantity=500,
)
# result: OrderResult(status="PARTIAL_FILLED", filled_qty=500, fill_price=10.5)
```

### 订单统计

```python
counts = adapter.get_order_count()
# {"total": 10, "active": 3}
```

### 断开连接

```python
adapter.disconnect_broker()
```

## C++ 枚举映射

OrderAdapter 内部维护 C++ 枚举值与 Python 字符串的映射:

| Python 字符串 | C++ 枚举值 |
|---|---|
| `side="BUY"` | `OrderSide.BUY` |
| `side="SELL"` | `OrderSide.SELL` |
| `order_type="MARKET"` | `OrderType.MARKET` |
| `order_type="LIMIT"` | `OrderType.LIMIT` |
| `order_type="STOP"` | `OrderType.STOP` |
| `status="PENDING_NEW"` | `OrderStatus.PENDING_NEW` |
| `status="NEW"` | `OrderStatus.NEW` |
| `status="PARTIAL_FILLED"` | `OrderStatus.PARTIAL_FILLED` |
| `status="FILLED"` | `OrderStatus.FILLED` |
| `status="CANCELLED"` | `OrderStatus.CANCELLED` |
| `status="PENDING_CANCEL"` | `OrderStatus.PENDING_CANCEL` |
| `status="REJECTED"` | `OrderStatus.REJECTED` |
| `status="EXPIRED"` | `OrderStatus.EXPIRED` |
| `status="SUSPENDED"` | `OrderStatus.SUSPENDED` |
| `time_in_force="DAY"` | `TimeInForce.DAY` |
| `time_in_force="IOC"` | `TimeInForce.IOC` |
| `time_in_force="GTC"` | `TimeInForce.GTC` |

**价格处理**: C++ 使用整数价格（万分之一元），OrderAdapter 自动进行 `price * 10000` 和 `price / 10000.0` 的转换。

**Result\<void\> 处理**: C++ `on_order_accepted`、`on_order_fill`、`on_order_rejected` 等方法返回 `Result<void>`，pybind11 尚未注册该类型的 Python 转换器，调用时会抛出 `TypeError`。但 C++ 侧的状态变更在异常抛出前已完成，因此 OrderAdapter 通过 `_cpp_call_void()` 辅助函数统一捕获该 `TypeError`，确保状态正确更新。

## 回测引擎集成

BacktestEngine 新增 `use_cpp_execution` 参数:

```python
engine = BacktestEngine(
    data_handler=data_handler,
    broker=broker,
    portfolio=portfolio,
    strategy=strategy,
    initial_cash=1_000_000.0,
    use_cpp_execution=True,       # 启用 C++ OrderManager 执行
)
```

**集成逻辑**:

1. `use_cpp_execution=True` 时，引擎在 `__init__` 中初始化 `OrderAdapter`
2. 如果 C++ 不可用，自动回退到 Python SimulatedBroker
3. 在 `_tick()` 中，根据模式选择执行路径:
   - C++ 模式: `_execute_orders_cpp()` → `OrderAdapter.submit_order()`
   - Python 模式: `SimulatedBroker.execute_orders()`（原有逻辑）
4. 回测结束时，断开 MockBroker 连接

**向后兼容**: `use_cpp_execution` 默认为 `False`，不影响现有回测逻辑。

## 纯 Python 回退模式

当 C++ `_quant_core` 模块不可用时，OrderAdapter 使用纯 Python 模拟:

- 内部维护 `_python_orders` 字典存储订单状态
- MARKET 单立即成交（与 C++ MockBroker 行为一致）
- LIMIT 单初始状态为 NEW，需通过 `simulate_fill()` 触发成交
- 订单 ID 自增分配

## 文件布局

```
py/src/quant_invest/execution/
├── __init__.py              # 导出 OrderAdapter, OrderInfo, OrderResult
└── order_adapter.py         # OrderAdapter 核心实现

py/src/quant_invest/backtest/
├── engine.py                # 新增 use_cpp_execution 参数和 _execute_orders_cpp()
├── broker.py                # SimulatedBroker（Python，未修改）
└── ...
```

## 使用示例

### 基本用法

```python
from quant_invest.execution.order_adapter import OrderAdapter

# 创建并连接
adapter = OrderAdapter()
adapter.connect_broker()

# 提交市价买单
result = adapter.submit_order(
    symbol="000001.SZ",
    side="BUY",
    order_type="MARKET",
    price=10.0,
    quantity=1000,
)
print(f"订单ID: {result.order_id}, 状态: {result.status}")

# 提交限价卖单
result2 = adapter.submit_order(
    symbol="600519.SH",
    side="SELL",
    order_type="LIMIT",
    price=1800.0,
    quantity=50,
)

# 模拟成交
fill = adapter.simulate_fill(result2.order_id, 1805.0, 50)
print(f"成交: {fill.filled_qty}@{fill.fill_price}")

# 查询活跃订单
active = adapter.get_active_orders()
print(f"活跃订单: {len(active)}")

# 断开连接
adapter.disconnect_broker()
```

### 在回测引擎中使用

```python
from quant_invest.backtest.engine import BacktestEngine

# 启用 C++ 执行模式
engine = BacktestEngine(
    data_handler=data_handler,
    broker=broker,
    portfolio=portfolio,
    strategy=strategy,
    use_cpp_execution=True,
)

result = engine.run()
```
