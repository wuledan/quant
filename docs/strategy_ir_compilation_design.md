# 详细方案：Python DSL → IR 编译 → C++ 执行

> 日期: 2026-05-21
> 状态: 方案设计
> 关联: discussion_2026_05_20.md, architecture_redesign.md

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│  Python 策略开发环境 (离线，不参与运行时)                       │
│                                                              │
│  ma_cross.py ──→ DSL Parser ──→ IR Compiler ──→ .graph 文件 │
│  (typed I/O)     (提取节点)    (类型推断+连接)   (序列化IR)    │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ .graph 文件 (JSON 格式，初期)
                              │
┌─────────────────────────────────────────────────────────────┐
│  C++ 后端引擎 (常驻进程)                                      │
│                                                              │
│  IR Loader ──→ FactorDAG::from_graph() ──→ FactorComputer   │
│  (反序列化)    (构图+拓扑排序)               (执行计算)        │
│                                                              │
│  StorageEngine ──→ EventBus ──→ WebSocketServer ──→ 前端    │
│  (数据源)        (事件分发)     (推送)                          │
└─────────────────────────────────────────────────────────────┘
```

## 2. IR 格式设计

### 2.1 格式选择：JSON（初期） → Protobuf（后期）

**初期用 JSON**：
- 开发调试方便，可直接阅读和手动编辑
- Python json 模块原生支持，C++ 有 nlohmann/json 或 rapidjson
- 不需要引入 protobuf 编译工具链
- 性能足够（策略编译是低频操作，不是每 bar 都编译）

**后期迁移 Protobuf**：
- 当策略数量多、需要跨进程传输时再迁移
- 迁移只需改序列化层，IR schema 不变

### 2.2 IR Schema

```json
{
  "strategy_name": "ma_cross",
  "version": 1,
  "created_at": "2026-05-21T10:00:00Z",

  "nodes": [
    {
      "id": "fast_ma",
      "op_type": "SMA",
      "inputs": {
        "price": { "type": "TimeSeries[float]", "source": "data.close" }
      },
      "outputs": {
        "value": { "type": "TimeSeries[float]" }
      },
      "params": {
        "period": 5
      }
    },
    {
      "id": "slow_ma",
      "op_type": "SMA",
      "inputs": {
        "price": { "type": "TimeSeries[float]", "source": "data.close" }
      },
      "outputs": {
        "value": { "type": "TimeSeries[float]" }
      },
      "params": {
        "period": 20
      }
    },
    {
      "id": "signal",
      "op_type": "CROSS_ABOVE",
      "inputs": {
        "fast": { "type": "TimeSeries[float]", "source": "node.fast_ma.value" },
        "slow": { "type": "TimeSeries[float]", "source": "node.slow_ma.value" }
      },
      "outputs": {
        "value": { "type": "Signal[float]" }
      },
      "params": {}
    }
  ],

  "edges": [
    { "from": "fast_ma", "from_port": "value", "to": "signal", "to_port": "fast" },
    { "from": "slow_ma", "from_port": "value", "to": "signal", "to_port": "slow" }
  ],

  "data_bindings": [
    { "data_source": "kline.close", "to_node": "fast_ma", "to_port": "price" },
    { "data_source": "kline.close", "to_node": "slow_ma", "to_port": "price" }
  ],

  "signal_handlers": [
    {
      "signal_node": "signal",
      "handler_type": "order",
      "params": {
        "buy_weight": 0.95,
        "sell_all": true
      }
    }
  ]
}
```

### 2.3 TypeSpec 定义

```
基础类型:
  - int64, float64, bool, string

复合类型:
  - TimeSeries[T]     → 时序数据，C++ 中对应 vector<T>
  - Signal[T]         → 信号值，T=float64 时: >0=看多, <0=看空, 0=无信号
  - Scalar[T]         → 单值（如当前持仓、现金）

数据源类型:
  - data.close        → 来自 StorageEngine 的 close 列
  - data.open, data.high, data.low, data.volume, data.amount
  - data.position     → 来自 Portfolio 的持仓数据
  - data.cash         → 来自 Portfolio 的现金数据
```

### 2.4 连接推断算法

```
1. 显式连接 (edges): 编译器从 DSL 中的 cross_above(fast_ma, slow_ma) 生成
2. 数据绑定 (data_bindings): Factor 的 input="close" → data_source="kline.close"
3. 类型推断:
   - 节点 A 输出 "value: TimeSeries[float]"
   - 节点 B 输入 "price: TimeSeries[float]"
   - 类型匹配 → 自动生成 edge: A.value → B.price
   - 同类型多输入时，用名称匹配（value→price 不匹配时需显式指定）
```

## 3. Python DSL v2 设计

### 3.1 核心变化

| 维度 | DSL v1 | DSL v2 |
|------|--------|--------|
| Factor 输入 | `input="close"` (字符串) | `input=DataField.close` (typed) |
| Factor 输出 | 无定义 | `output="value"` (默认) |
| SignalExpr 输入 | operands 是 DAGNode | operands 带 port 名称 |
| 编译产出 | 直接注册 C++ 对象 | 输出 .graph IR 文件 |
| 运行时 | Python on_signal 回调 | C++ SignalHandler 执行 |

### 3.2 DSL v2 代码示例

```python
from quant_invest.strategy.dsl2 import Strategy, Factor, Signal, DataField
from quant_invest.strategy.dsl2 import cross_above, cross_below, threshold

@strategy("ma_cross")
class MACross(Strategy):
    # Factor: typed input/output
    fast_ma = Factor(
        op="SMA",
        inputs={"price": DataField.close},
        outputs={"value": "TimeSeries[float]"},
        params={"period": 5}
    )
    slow_ma = Factor(
        op="SMA",
        inputs={"price": DataField.close},
        outputs={"value": "TimeSeries[float]"},
        params={"period": 20}
    )
    # Signal: typed input/output, port names for connection
    signal = cross_above(
        fast=MACross.fast_ma.output("value"),
        slow=MACross.slow_ma.output("value"),
    )

    # Signal handler: 简单的订单生成规则
    @on_signal("signal")
    def handle_signal(self, ctx):
        if ctx.signal > 0:
            ctx.order(side=BUY, weight=0.95)
        elif ctx.signal < 0:
            ctx.order(side=SELL, weight=1.0)
```

### 3.3 DSL v2 核心类

```python
class DataField:
    """数据源字段枚举"""
    open = "data.open"
    high = "data.high"
    low = "data.low"
    close = "data.close"
    volume = "data.volume"
    amount = "data.amount"
    position = "data.position"
    cash = "data.cash"

class PortRef:
    """节点端口引用 — 用于连接推断"""
    node_id: str      # 所属节点 ID
    port_name: str     # 输出端口名
    port_type: str     # 类型标识

class Factor(DAGNode):
    op: str
    inputs: dict[str, DataField | PortRef]  # typed inputs
    outputs: dict[str, str]                  # output name → type
    params: dict[str, Any]

    def output(self, name: str = "value") -> PortRef:
        return PortRef(self.node_id, name, self.outputs[name])

class Signal(DAGNode):
    op: str
    inputs: dict[str, PortRef]               # typed inputs with port names
    outputs: dict[str, str]
    params: dict[str, Any]

    def output(self, name: str = "value") -> PortRef:
        return PortRef(self.node_id, name, self.outputs[name])
```

### 3.4 信号组合器 v2

```python
def cross_above(**operands: PortRef) -> Signal:
    """金叉信号 — operands 必须有命名端口"""
    return Signal(
        op="CROSS_ABOVE",
        inputs=operands,
        outputs={"value": "Signal[float]"},
        params={}
    )

def cross_below(**operands: PortRef) -> Signal:
    return Signal(
        op="CROSS_BELOW",
        inputs=operands,
        outputs={"value": "Signal[float]"},
        params={}
    )

def threshold(signal: PortRef, value: float) -> Signal:
    return Signal(
        op="THRESHOLD",
        inputs={"signal": signal},
        outputs={"value": "Signal[float]"},
        params={"threshold": value}
    )
```

## 4. IR 编译器设计

### 4.1 编译流程

```
Strategy 类 → 提取 Factor/Signal 声明 → 构建节点列表 → 推断连接 → 生成 IR JSON → 写入 .graph 文件
```

### 4.2 编译器核心逻辑

```python
class IRCompiler:
    def compile(self, strategy_cls: type[Strategy]) -> dict:
        # 1. 提取声明
        nodes = self._extract_nodes(strategy_cls)

        # 2. 生成节点 IR
        node_defs = []
        for node in nodes:
            node_def = self._build_node_def(node)
            node_defs.append(node_def)

        # 3. 推断连接
        edges = self._infer_edges(nodes)
        data_bindings = self._infer_data_bindings(nodes)

        # 4. 提取信号处理器
        signal_handlers = self._extract_handlers(strategy_cls)

        # 5. 组装 IR
        return {
            "strategy_name": strategy_cls._strategy_name,
            "version": 1,
            "nodes": node_defs,
            "edges": edges,
            "data_bindings": data_bindings,
            "signal_handlers": signal_handlers,
        }

    def _infer_edges(self, nodes) -> list[dict]:
        """从 Signal.inputs 中的 PortRef 生成 edge 列表"""
        edges = []
        for node in nodes:
            if isinstance(node, Signal):
                for port_name, port_ref in node.inputs.items():
                    if isinstance(port_ref, PortRef):
                        edges.append({
                            "from": port_ref.node_id,
                            "from_port": port_ref.port_name,
                            "to": node.node_id,
                            "to_port": port_name,
                        })
        return edges

    def _infer_data_bindings(self, nodes) -> list[dict]:
        """从 Factor.inputs 中的 DataField 生成 data_binding 列表"""
        bindings = []
        for node in nodes:
            if isinstance(node, Factor):
                for port_name, source in node.inputs.items():
                    if isinstance(source, DataField):
                        bindings.append({
                            "data_source": source.value,
                            "to_node": node.node_id,
                            "to_port": port_name,
                        })
        return bindings

    def write_graph(self, strategy_cls: type[Strategy], output_path: str) -> str:
        """编译并写入 .graph 文件"""
        ir = self.compile(strategy_cls)
        with open(output_path, 'w') as f:
            json.dump(ir, f, indent=2)
        return output_path
```

## 5. C++ IR 加载器设计

### 5.1 IR 加载流程

```
.graph 文件 → JSON 解析 → NodeDef 列表 → FactorRegistry 注册 → FactorDAG::from_graph() → FactorComputer
```

### 5.2 C++ IR 数据结构

```cpp
// ir_graph.h
namespace quant::ir {

struct TypeSpec {
    std::string base_type;    // "TimeSeries", "Signal", "Scalar"
    std::string inner_type;   // "float", "int64", "bool"
};

struct PortDef {
    std::string name;
    TypeSpec    type;
    std::string source;       // "data.close" or "node.fast_ma.value"
};

struct NodeDef {
    std::string id;
    std::string op_type;      // "SMA", "CROSS_ABOVE", "THRESHOLD"
    std::unordered_map<std::string, PortDef> inputs;
    std::unordered_map<std::string, PortDef> outputs;
    std::unordered_map<std::string, double>  params;  // numeric params
};

struct EdgeDef {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
};

struct DataBinding {
    std::string data_source;  // "kline.close"
    std::string to_node;
    std::string to_port;
};

struct SignalHandler {
    std::string signal_node;
    std::string handler_type; // "order", "alert"
    std::unordered_map<std::string, double> params;
};

struct StrategyGraph {
    std::string strategy_name;
    uint32_t    version;
    std::vector<NodeDef>         nodes;
    std::vector<EdgeDef>         edges;
    std::vector<DataBinding>     data_bindings;
    std::vector<SignalHandler>   signal_handlers;

    // Load from JSON file
    static StrategyGraph load_from_file(const std::string& path);

    // Load from JSON string
    static StrategyGraph load_from_json(const std::string& json_str);

    // Validate: check type compatibility, no cycles, all ports connected
    bool validate(std::string& error_msg) const;
};

}  // namespace quant::ir
```

### 5.3 FactorDAG::from_graph() 工厂方法

```cpp
// 在 factor_dag.h 中新增
class FactorDAG {
public:
    // ... existing methods ...

    // 从 IR StrategyGraph 构建因子计算图
    static std::unique_ptr<FactorComputer> from_graph(
        const ir::StrategyGraph& graph,
        StorageEngine& store);

private:
    // 根据 NodeDef.op_type 查找注册的算子
    static FactorComputeFn resolve_op(const std::string& op_type,
                                       const std::unordered_map<std::string, double>& params);
};
```

### 5.4 from_graph() 实现逻辑

```cpp
std::unique_ptr<FactorComputer> FactorDAG::from_graph(
    const ir::StrategyGraph& graph,
    StorageEngine& store) {

    auto registry = std::make_unique<FactorRegistry>();

    // 1. 注册内置因子
    BuiltInFactors::register_all(*registry);

    // 2. 为每个 IR NodeDef 注册计算函数
    for (const auto& node : graph.nodes) {
        auto compute_fn = resolve_op(node.op_type, node.params);
        FactorMeta meta;
        meta.name = node.id;
        meta.inputs = /* 从 node.inputs 提取 */;
        meta.outputs = /* 从 node.outputs 提取 */;
        registry->register_factor(meta, compute_fn);
    }

    // 3. 构建 DAG
    auto dag = std::make_unique<FactorDAG>(registry.get());
    // 根据 edges 设置依赖关系
    for (const auto& edge : graph.edges) {
        dag->add_dependency(edge.to_node, edge.from_node);
    }
    dag->build();

    // 4. 创建 FactorComputer
    return std::make_unique<FactorComputer>(
        std::move(registry), std::move(dag));
}
```

### 5.5 算子注册表 (OpRegistry)

```cpp
// op_registry.h — 算子注册表，将 op_type 映射到计算函数
namespace quant::factor {

class OpRegistry {
public:
    using OpFactory = std::function<FactorComputeFn(
        const std::unordered_map<std::string, double>& params)>;

    // 注册算子工厂
    static void register_op(const std::string& op_type, OpFactory factory);

    // 查找算子工厂
    static OpFactory find(const std::string& op_type);

    // 列出所有注册的算子
    static std::vector<std::string> list_ops();

private:
    static std::unordered_map<std::string, OpFactory>& ops();
};

// 内置算子注册
void register_builtin_ops();  // SMA, EMA, RSI, MACD, BOLL, CROSS_ABOVE, etc.

}  // namespace quant::factor
```

### 5.6 信号处理器 (SignalHandler)

```cpp
// signal_handler.h — 信号触发后的动作执行
namespace quant::strategy {

class ISignalHandler {
public:
    virtual ~ISignalHandler() = default;
    virtual void handle(double signal_value, const SignalContext& ctx) = 0;
};

class OrderSignalHandler : public ISignalHandler {
    // 根据 signal_value > 0 / < 0 生成买入/卖出订单
    // params: buy_weight, sell_weight, etc.
};

class AlertSignalHandler : public ISignalHandler {
    // 发送风控告警
};

}  // namespace quant::strategy
```

## 6. 数据流全链路

### 6.1 回测模式

```
1. Python 编写策略 → IR 编译器 → .graph 文件
2. C++ BacktestRunner 加载 .graph → FactorDAG::from_graph()
3. StorageEngine.query_kline() → 输入数据 (零拷贝)
4. FactorComputer.compute_all() → 因子值 + 信号值
5. SignalHandler.handle() → 生成订单
6. RiskEngine.check_order() → 风控检查
7. SimulatedBroker.execute() → 成交
8. Portfolio.update() → 净值曲线
9. 结果写入 StorageEngine → 前端查询展示
```

### 6.2 实盘模式

```
1. DataIngestor → StorageEngine.store_kline() → EventBus.publish(KlineEvent)
2. EventBus → FactorComputer.increment() → 因子增量更新
3. FactorComputer → SignalNode.evaluate() → 信号值
4. SignalHandler → OrderManager.submit() → 订单
5. RiskEngine → 检查 → 通过/拒绝
6. OrderManager → 券商接口 → 成交回报
7. EventBus.publish(OrderReportEvent) → WebSocketServer → 前端
```

## 7. 与现有代码的改造映射

| 现有组件 | 改造动作 | 新组件 |
|----------|----------|--------|
| `dsl.py` (Factor/SignalExpr/DAGNode) | 重写为 typed I/O 版本 | `dsl2.py` (Factor/Signal/PortRef/DataField) |
| `compiler.py` (StrategyCompiler) | 重写为 IR 编译器 | `ir_compiler.py` (IRCompiler) |
| `compiler.py` (PythonExecutor) | 废弃 | C++ FactorComputer 直接执行 |
| `compiler.py` (_sma/_ema/_rsi 等回退) | 废弃 | C++ BuiltInFactors + OpRegistry |
| `hot_reload.py` (watchfiles) | 改为编译 IR → C++ 热加载 | `ir_hot_reload.py` |
| `FactorDAG` (C++) | 增加 from_graph() | FactorDAG + OpRegistry |
| `FactorRegistry` (C++) | 不变，但增加 OpRegistry 查找层 | FactorRegistry + OpRegistry |
| `FactorComputer` (C++) | 不变 | FactorComputer |
| `BuiltInFactors` (C++) | 扩展，增加信号算子 | BuiltInFactors + SignalOps |
| `pybind/py_factor.cc` | 增加 IR 加载接口 | py_factor + py_ir |
| `StorageEngine` (C++) | 修复 codec bug | StorageEngine (fixed) |
| `DataIngestor` (C++) | 补全网络协程 | DataIngestor (complete) |
| `pipeline/kline_pipeline.py` | 废弃，C++ EventBus 全链路 | C++ StrategyEngine |
| `storage/adapter.py` | 简化，直接用 StorageEngine | StorageEngine (direct) |
| `strategy/registry.py` | 保留，增加 IR 路径管理 | StrategyRegistry + IRPath |
| `strategy/event_bus_bridge.py` | 废弃 | C++ EventBus 直接 |

## 8. 新增 C++ 组件

| 组件 | 文件 | 职责 |
|------|------|------|
| IR 数据结构 | `cpp/quant/ir/ir_graph.h/.cc` | StrategyGraph, NodeDef, EdgeDef, JSON 加载 |
| 算子注册表 | `cpp/quant/factor/op_registry.h/.cc` | op_type → compute_fn 工厂映射 |
| 信号算子 | `cpp/quant/factor/signal_ops.h/.cc` | CROSS_ABOVE, CROSS_BELOW, THRESHOLD, AND, OR, NOT |
| 信号处理器 | `cpp/quant/strategy/signal_handler.h/.cc` | OrderSignalHandler, AlertSignalHandler |
| 策略引擎 | `cpp/quant/strategy/strategy_engine.h/.cc` | 加载 IR → 构图 → 注册 EventBus → 执行 |
| 回测引擎 | `cpp/quant/backtest/backtest_runner.h/.cc` | 从 StorageEngine 读数据 → 执行策略 → 输出净值 |
| 模拟券商 | `cpp/quant/backtest/simulated_broker.h/.cc` | 订单模拟执行、滑点、手续费 |
| 组合管理 | `cpp/quant/portfolio/portfolio.h/.cc` | 持仓、现金、净值追踪 |
| 常驻服务 | `cpp/quant/service/service_main.cc` | main() 启动所有组件 |
| IR pybind | `cpp/quant/pybind/py_ir.cc` | IR 加载、策略编译 Python 接口 |

## 9. 新增 Python 组件

| 组件 | 文件 | 职责 |
|------|------|------|
| DSL v2 | `py/src/quant_invest/strategy/dsl2.py` | typed I/O 的 Factor/Signal/DataField/PortRef |
| IR 编译器 | `py/src/quant_invest/strategy/ir_compiler.py` | Strategy → IR JSON 编译 |
| IR 热加载 | `py/src/quant_invest/strategy/ir_hot_reload.py` | 编译 IR → 通知 C++ 热加载 |
| 策略注册 v2 | `py/src/quant_invest/strategy/registry_v2.py` | 策略注册 + IR 路径管理 |

## 10. 策略中心 (SQLite)

### 10.1 表结构

```sql
CREATE TABLE strategies (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT UNIQUE NOT NULL,
    source_path TEXT NOT NULL,          -- .py 源文件路径
    graph_path  TEXT,                   -- 编译后的 .graph 文件路径
    status      TEXT DEFAULT 'draft',   -- draft/active/paused/deleted
    params_json TEXT DEFAULT '{}',
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE strategy_runs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    strategy_id INTEGER REFERENCES strategies(id),
    run_type    TEXT,                    -- backtest/live
    start_time  TIMESTAMP,
    end_time    TIMESTAMP,
    status      TEXT,                    -- running/completed/failed
    result_path TEXT,                    -- 回测结果文件路径
    metrics_json TEXT                    -- 收益率、夏普等指标
);
```

### 10.2 API

```
POST   /api/strategies              — 注册策略 (上传 .py → 编译 → 存储)
GET    /api/strategies              — 列出所有策略
GET    /api/strategies/:id          — 获取策略详情
PUT    /api/strategies/:id          — 更新策略参数
DELETE /api/strategies/:id          — 删除策略
POST   /api/strategies/:id/activate — 激活策略 (C++ 热加载)
POST   /api/strategies/:id/pause    — 暂停策略
POST   /api/strategies/:id/backtest — 触发回测
GET    /api/strategies/:id/runs     — 获取回测运行列表
GET    /api/strategies/:id/runs/:rid — 获取回测结果
```

## 11. 前端闭环

```
1. 策略列表页: GET /api/strategies → 展示所有策略 + 状态
2. 策略详情页: GET /api/strategies/:id → 展示策略代码 + DAG 图可视化
3. 回测触发: POST /api/strategies/:id/backtest → 参数配置 → 开始回测
4. 回测结果: GET /api/strategies/:id/runs/:rid → 净值曲线 + 指标
5. 实时监控: WebSocket → 策略运行状态 + 订单 + 风控告警
```

## 12. 价格处理约定

```
C++ 内部: int64 (price * 10000)，避免浮点精度问题
StorageEngine: 存 int64，Gorilla codec
IR 传输: float64 (Python 侧)
pybind11 边界: int64 → float64 (÷10000)，float64 → int64 (×10000)
FactorComputer: 输入 float64，输出 float64 (内部可转 int64 计算)
```