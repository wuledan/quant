# 架构重设计：C++ 常驻引擎 + Python 策略编译

> 日期: 2026-05-20 (更新)
> 状态: 设计阶段
> 前置文档: architecture_cpp_core.md, architecture_python.md
> 讨论记录: discussion_2026_05_20.md

## 1. 背景与问题

### 1.1 当前架构

```
Python FastAPI (uvicorn :8000)
  ├── 数据调度: asyncio 定时拉取 Yahoo Finance → Parquet 文件
  ├── 回测引擎: 每次提交 → new BacktestEngine → 重新拉数据 → Python 循环
  ├── 策略管理: Python 装饰器注册, 修改需重启
  └── 前端通信: FastAPI WebSocket

C++ 引擎 (pybind11 _quant_core)
  ├── StorageEngine: 列存 + 压缩 + 持久化 (已实现, 未接入)
  ├── EventBus: 发布订阅 + 协程分发 (已实现, 未接入)
  ├── FactorDAG: 因子依赖图 + 拓扑并行 (已实现, 未接入)
  ├── OrderManager: 订单生命周期 (已实现, 未接入)
  ├── SchedulerService: cron + DAG 调度 (已实现, 未接入)
  └── WebSocketServer: io_uring (已实现, 未接入)
```

### 1.2 核心问题

| 问题 | 根因 | 影响 |
|------|------|------|
| 回测每次重新拉数据 | 没有常驻数据层, 回测引擎和数据采集割裂 | 慢, 重复网络IO |
| 策略迭代慢 | 改策略 → 重启后端 → 等数据加载 | 开发效率低 |
| C++ 引擎闲置 | StorageEngine/EventBus/FactorDAG/OrderManager 全部已实现但未接入 | 性能浪费 |
| 数据不在引擎里 | 散落在 Parquet + Python 内存缓存, C++ StorageEngine 空转 | 查询慢, 无法零拷贝 |
| 单进程模型 | Python 单进程承担所有职责 | 无法利用多核, GC 停顿影响实时性 |
| **Python 驱动 C++** | **pybind11 让 Python 做主循环, C++ 只是被调库** | **C++ 引擎无法自主运行, Python 成为性能瓶颈** |

### 1.3 架构方向变更 (2026-05-20)

**原方案**: Python 主进程 + pybind11 调用 C++ 引擎
**新方案**: Python 只负责策略编写和编译，产出 IR（图拓扑描述），C++ 后端加载 IR → 构图 → 执行

**用户原话**: "这不是我所期望的结构，考虑更换实现，我所预期的方式是，python 生成的策略计算图，经过编译后,可能是一个c++代码，或者是一段protobuf类似的图拓补描述，后端引擎加载相应编译后产出-> 构图-> 执行"

## 2. 目标架构

```
┌──────────────────────────────────────────────────────────────┐
│                   C++ 后端服务 (常驻进程)                       │
│                                                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐     │
│  │Storage   │  │EventBus  │  │Scheduler │  │WebSocket│     │
│  │Engine    │  │          │  │Service   │  │Server   │     │
│  │          │  │ Kline ──→│  │ cron+dag │  │ :8080   │     │
│  │ store()  │  │ Factor──→│  │          │  │         │     │
│  │ query()  │  │ Signal──→│  │          │  │ push    │     │
│  │ flush()  │  │ Order───→│  │          │  │         │     │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬────┘     │
│       │              │              │              │          │
│  ┌────┴──────────────┴──────────────┴──────────────┴────┐   │
│  │              IR Loader + FactorDAG Builder            │   │
│  │  load(IR) → NodeDef → FactorRegistry → FactorDAG     │   │
│  │  → FactorComputer.compute() → 结果                    │   │
│  └───────────────────────────────────────────────────────┘   │
│                        ^ 加载编译产物                          │
└────────────────────────┼────────────────────────────────────┘
                         │ .graph (IR 文件)
┌────────────────────────┴────────────────────────────────────┐
│              Python 策略开发 + 编译 (离线工具)                  │
│                                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│  │策略脚本   │  │策略编译器 │  │IR 输出    │                  │
│  │.py DSL   │→ │AST→IR    │→ │.graph    │                  │
│  │typed I/O │  │类型推断   │  │protobuf  │                  │
│  └──────────┘  └──────────┘  └──────────┘                  │
│                                                              │
│  Python 只负责编写和编译，不参与运行时计算                        │
└─────────────────────────────────────────────────────────────┘
```

### 关键变化：Python 不再驱动 C++

| 维度 | 旧方案 | 新方案 |
|------|--------|--------|
| 运行时 Python 角色 | 主循环，通过 pybind11 调 C++ | 不参与，仅编译期使用 |
| 策略执行 | Python on_bar → C++ 因子 | C++ 加载 IR → 构图 → 自主执行 |
| 因子计算 | Python rolling / C++ fallback | C++ FactorDAG 全部执行 |
| 信号生成 | Python strategy.on_signal() | IR 中定义信号节点，C++ 执行 |
| 策略热加载 | Python watchfiles → 重新 import | 编译新 IR → C++ 热加载 .graph |
| 数据流 | Python 拉数据 → C++ 存储 | C++ DataIngestor → StorageEngine |

## 3. 组件设计

### 3.1 C++ 数据引擎 (StorageEngine 接入)

**职责**: 常驻数据存储, 替代 Python Parquet 缓存

**启动流程**:
1. `StorageEngine` 加载本地 Parquet 初始化列存
2. `EventBus` 启动协程分发循环
3. `SchedulerService` 注册定时任务 (交易时段每5分钟增量采集)
4. `WebSocketServer` 在 8080 端口推送实时数据

**交易时段**:
- 数据采集协程拉取行情 → `store_kline_batch()` 写入 StorageEngine → `publish(KlineEvent)` 广播
- EventBus 分发 KlineEvent → FactorDAG 计算因子 → `publish(FactorUpdateEvent)`

**数据查询**:
- `query_kline(symbol, type, field, range)` → 零拷贝返回 numpy array
- 回测直接从 StorageEngine 读, 无需重复拉网

**pybind11 接口**:
```python
import _quant_core

engine = _quant_core.StorageEngine(cache_budget_mb=512, data_dir="./data")
engine.store_kline_batch(symbol, kline_type, rows)  # 写入
result = engine.query_kline(symbol, kline_type, field, time_range)  # 查询
# result.timestamps: numpy int64 array
# result.values: numpy float64 array (零拷贝)
```

### 3.2 Python 策略编译 → IR → C++ 加载执行

**职责**: Python 编写策略 → 编译为图拓扑 IR → C++ 后端加载执行

**策略开发流程**:
```
编辑 ma_cross.py → 保存 → 编译器提取节点定义 → 类型推断 → 生成 .graph IR → C++ 后端热加载
```

**策略声明式定义 (typed I/O)**:
```python
from quant_invest.strategy import Strategy, Factor, cross_above

@strategy("ma_cross")
class MACross:
    # 每个节点有明确的输入/输出类型定义
    fast_ma = Factor("SMA", period=5, input="close")   # output: TimeSeries[float]
    slow_ma = Factor("SMA", period=20, input="close")  # output: TimeSeries[float]
    signal = cross_above(fast_ma, slow_ma)             # input: 2×TimeSeries[float], output: Signal

    def on_signal(self, ctx):
        if self.signal > 0:
            ctx.order(symbol=ctx.symbol, side=BUY, quantity=ctx.cash * 0.95 / ctx.price)
        elif self.signal < 0:
            ctx.order(symbol=ctx.symbol, side=SELL, quantity=ctx.position)
```

**编译产出 (IR)**:
```protobuf
message StrategyGraph {
  repeated NodeDef nodes = 1;
  repeated EdgeDef edges = 2;    // 自动推断 + 显式覆盖
  repeated ParamBinding params = 3;
}

message NodeDef {
  string name = 1;
  string op_type = 2;            // "SMA", "CROSS_ABOVE", "THRESHOLD"
  map<string, TypeSpec> inputs = 3;
  map<string, TypeSpec> outputs = 4;
  map<string, ParamValue> params = 5;
}

message EdgeDef {
  string source_node = 1;
  string source_output = 2;
  string target_node = 3;
  string target_input = 4;
}
```

**C++ 后端加载**:
```cpp
auto graph = StrategyGraph::load_from_file("ma_cross.graph");
auto dag = FactorDAG::from_graph(graph);  // 根据 NodeDef + EdgeDef 构图
dag.compute(store, time_range);            // 执行
```

**图连接推断**:
- 节点 A 输出 `value: TimeSeries[float]`
- 节点 B 输入 `price: TimeSeries[float]`
- 类型匹配 → 自动连接 A.value → B.price
- 同类型多输出时，用显式名称匹配

### 3.3 回测引擎 (C++ 全链路执行)

**职责**: 高性能回测, 数据零拷贝, 因子 C++ 并行计算

**数据流**:
```
StorageEngine.query_kline() → numpy array (零拷贝)
  → IR Loader → FactorDAG.compute() → 因子值 (C++ 并行)
  → SignalNode.evaluate() → 信号 (C++ 执行)
  → C++ SimulatedBroker.execute() → 成交
  → Portfolio.update() → 净值
```

**对比当前**:
- 当前: Python 拉数据 → Python rolling → Python on_bar → Python broker
- 目标: C++ 读数据 → C++ 因子 → C++ 信号 → C++ 订单

### 3.4 实时推送 (C++ WebSocketServer)

**职责**: 替代 FastAPI WebSocket, 低延迟推送

**频道**:
- `market`: KlineEvent → 前端 K 线更新
- `factor`: FactorUpdateEvent → 前端因子值更新
- `order`: OrderReportEvent → 前端订单状态
- `risk`: RiskAlertEvent → 前端风控告警

## 4. 对比

| 维度 | 当前 | 目标 |
|------|------|------|
| 数据存储 | Python Parquet + 内存 dict | C++ StorageEngine 列存 + LRU 缓存 |
| 数据采集 | Python asyncio 定时拉 | C++ SchedulerService 协程 + io_uring |
| 因子计算 | Python rolling | C++ FactorDAG 拓扑并行 |
| 策略执行 | Python on_bar 循环 | C++ IR 加载 → FactorDAG 自主执行 |
| 策略迭代 | 改代码 → 重启 | 改代码 → 编译 IR → C++ 热加载 |
| 回测数据 | 每次拉网/读 Parquet | 直接读 StorageEngine 零拷贝 |
| 实时推送 | FastAPI WS | C++ WebSocketServer io_uring |
| 进程模型 | Python 单进程 | C++ 常驻 + Python 仅编译期 |
| Python 角色 | 运行时主循环 | 编译期工具，不参与运行时 |

## 5. 实施路径

### P0: C++ 常驻服务 + StorageEngine 接入
- 修复 StorageEngine codec bug (#142)
- 实现 C++ 常驻服务 main() (#144)
- 实现 DataIngestor 网络协程 (#145)
- 验证: C++ 进程常驻运行，数据自动采集写入 StorageEngine

### P1: Python DSL → IR 编译器
- 设计 IR schema (Protobuf/FlatBuffers/JSON)
- 重写 Python DSL v2: typed input/output 节点定义
- 实现类型推断和连接匹配算法
- 实现 Python → IR 编译器
- 验证: Python 策略 → 编译输出 .graph IR 文件

### P2: C++ IR 加载器 + FactorDAG 执行
- 实现 C++ IR Loader: .graph → NodeDef → FactorRegistry 查找
- 实现 FactorDAG::from_graph() 工厂方法
- 实现 C++ 信号节点执行
- 回测全链路走 C++ (StorageEngine → FactorDAG → Signal → Order)
- 验证: C++ 加载 IR → 构图 → 执行回测

### P3: 策略中心 + 前端闭环
- 策略注册中心 SQLite (#143)
- 策略提交注册 API (#147)
- 前端策略选择 + 回测触发 + 结果展示 (#146)
- 验证: 前端提交策略 → 编译 → 注册 → 触发回测 → 查看结果

### P4: 实时交易链路
- C++ WebSocketServer 替代 FastAPI WS
- EventBus 全链路: Kline → Factor → Signal → Risk → Order
- 风控: RiskEngine 检查订单 → RiskAlertEvent
- 验证: 端到端延迟 < 1ms (同进程内)

## 6. Agent 团队分工

| Agent | 方向 | 负责组件 |
|-------|------|----------|
| storage-engine-expert | C++ 存储引擎 | P0: StorageEngine codec 修复, DataIngestor 网络协程 |
| storage-engine-expert | C++ 存储引擎 | P2: IR Loader, FactorDAG::from_graph() |
| go-backend-expert | 后端架构 | P0: C++ 常驻服务进程 |
| go-backend-expert | 后端架构 | P1: Python DSL v2, IR 编译器 |
| frontend-expert | 前端 | P3: 策略选择 + 回测触发 + 结果展示 |
| backend-testing-expert | 后端测试 | 各阶段单元测试, 集成测试, 性能基准测试 |
| 总架构师 | 架构协调 | 任务分配, Review, 推送, 架构文档更新 |

## 7. 验收标准

- [ ] StorageEngine codec bug 修复，store_kline/query_kline 数据一致
- [ ] C++ 常驻服务进程可启动，StorageEngine + EventBus + Scheduler 运行正常
- [ ] DataIngestor 网络协程可连接数据源、接收数据、写入 StorageEngine
- [ ] Python DSL v2 支持 typed input/output 节点定义
- [ ] Python → IR 编译器可输出 .graph 文件
- [ ] C++ IR Loader 可加载 .graph → 构造 FactorDAG → 执行
- [ ] 回测全链路走 C++，无需 Python 运行时参与
- [ ] 策略注册中心 SQLite CRUD 可用
- [ ] 前端可提交策略 → 触发回测 → 查看结果
- [ ] 所有组件有单元测试，集成测试通过
