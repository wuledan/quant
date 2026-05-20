# 系统讨论记录：架构重设计 & 待办事项

> 日期: 2026-05-20
> 状态: 已确认，待实施
> 关联文档: architecture_redesign.md, architecture_cpp_core.md, architecture_python.md

## 1. StorageEngine Codec Bug

### 问题描述
`StorageEngine::store_kline()` 对所有字段统一使用 Delta(int64) 编码，但价格字段（open/high/low/close）在 `query_kline()` 解压时使用 `span<double>` 读取，导致编码/解码类型不匹配。

### 具体位置
- `cpp/quant/storage/storage_engine.cc` — `store_kline()`: 所有字段用 `ColumnBlock::Codec::kDelta` + `span<const int64_t>`
- `cpp/quant/storage/storage_engine.cc` — `query_kline()`: 解压用 `span<double>` 读取

### 影响
- 通过 `StorageEngine::store_kline()` 写入的数据，用 `query_kline()` 读回会得到错误值
- Python adapter (`storage/adapter.py`) 已绕过此 bug：直接调用 `TimeSeriesStore::put()` 并手动选择 codec（价格用 Gorilla+compress_double，成交量用 Delta+compress_int64）

### 修复方案
- `store_kline()` 中价格字段改用 Gorilla codec + double 编码
- `query_kline()` 解压逻辑与写入 codec 对齐
- 或统一为 int64 编码（C++ 内部用整数价格，1/10000元），query 时转换

### 任务编号
- #142: Fix StorageEngine store_kline/query_kline codec mismatch

---

## 2. 策略中心设计

### 需求
- 策略注册中心：管理策略的注册、查询、启停
- 初期用 SQLite 存储，后续可迁移到 etcd（支持分布式）
- 与热加载机制集成：注册策略 → 文件监视 → 自动重载

### 设计要点
- SQLite 表结构：strategy_id, name, source_path, status(active/paused/deleted), params_json, created_at, updated_at
- CRUD API：register / list / get / update / delete / activate / deactivate
- 热加载集成：注册时启动文件监视，删除时停止监视
- etcd 迁移路径：SQLite → etcd watch 机制，接口不变

### 任务编号
- #143: Strategy registration center (SQLite + CRUD + hot reload integration)

---

## 3. ML / 因子挖掘 — 延后决策

### 结论
ML 和因子挖掘能力暂不实施，等统一模型训练框架时再考虑。

### 原因
- 这是一个较大的工程，需要特征工程、模型训练、预测集成等完整管线
- 当前优先级：先把 C++ 后端引擎跑起来，策略编译执行链路打通
- 后续统一规划时，可与因子检验（IC/IR、分层回测）一起设计

### 影响范围
- P2-T2 (Factor Engine) 的 ML 部分暂缓
- 因子计算引擎（C++ FactorDAG）继续推进，但不涉及 ML 模型推理节点

---

## 4. 网络协程现状与改动需求

### 当前状态
- `CoIouring` (io_uring 协程 I/O) 已实现：`co_connect`, `co_send`, `co_recv`, `co_accept`
- `TcpConnection` 已实现协程方法：`co_connect()`, `co_send()`, `co_recv()`
- `GlobalExecutor` + `GlobalCoIouring` 进程级单例已实现
- `DataIngestor` 的网络协程部分**未完成**：`start()` 方法是空协程，`connect_loop`/`receive_loop` 未实现

### 需要的改动
1. **DataIngestor 网络协程**：实现 `start()` 中的连接循环 + 数据接收循环
   - `connect_loop()`: co_connect → 心跳 → 断线重连
   - `receive_loop()`: co_recv → parse_kline → store_kline → publish_kline_event
2. **C++ 常驻服务进程**：需要一个 main() 启动 GlobalExecutor + CoIouring + StorageEngine + EventBus
3. **心跳机制**：DataSourceConfig 已有 heartbeat_interval_ms 字段，需实现

### 任务编号
- #144: C++ resident service process (GlobalExecutor + CoIouring + StorageEngine + EventBus)
- #145: Complete DataIngestor network coroutines (co_connect + co_recv + heartbeat)

---

## 5. 系统部署架构

### 目标
前端 + 常驻 C++ 后端服务

### 部署形态
```
┌──────────────┐     WebSocket      ┌──────────────────────────────┐
│  前端 (React) │ ←───────────────→ │  C++ 后端服务 (常驻进程)       │
│  :3000       │     HTTP REST      │  :8080 (WS) + :8000 (REST)   │
└──────────────┘                    │                               │
                                    │  StorageEngine (列存+压缩)     │
                                    │  EventBus (发布订阅)           │
                                    │  FactorDAG (因子计算图)        │
                                    │  SchedulerService (定时调度)   │
                                    │  WebSocketServer (实时推送)    │
                                    │  DataIngestor (数据采集)       │
                                    │  OrderManager (订单管理)       │
                                    │  RiskEngine (风控)            │
                                    └──────────────────────────────┘
```

### 功能
- 前端选择已注册策略 → 触发回测 → 显示结果
- Python 开发策略 → 提交注册至后端 → 前端可见
- C++ 后端常驻运行，数据常驻内存，无需每次回测重新加载

### 任务编号
- #144: C++ resident service process
- #146: Frontend strategy selection + backtest trigger + results display
- #147: Strategy submission/registration API (upload → validate → register → hot reload)

---

## 6. 核心架构重设计：Python DSL → 编译 → C++ 执行

### 6.1 问题

当前架构是 Python 为主进程，通过 pybind11 调用 C++。用户明确表示**这不是期望的结构**。

**用户原话**: "这不是我所期望的结构，考虑更换实现"

### 6.2 期望架构

```
Python DSL (策略编写)  →  编译器  →  IR (图拓扑描述)  →  C++ 后端加载  →  构图  →  执行
```

**核心思想**:
- Python 只负责策略编写和编译，不参与运行时计算
- 策略计算图编译为一种中间表示（IR），可以是：
  - C++ 代码（编译为 .so 动态加载）
  - Protobuf/FlatBuffers 类似的图拓扑描述（序列化格式）
- C++ 后端加载 IR → 构造 FactorDAG → 执行
- 图中计算节点有明确的输入/输出数据结构定义
- 图的连接关系通过输入/输出类型匹配自动推断

### 6.3 设计要点

**节点定义**:
```python
# 每个计算节点有 typed input/output
class NodeDef:
    name: str
    op_type: str          # e.g. "SMA", "CROSS_ABOVE", "THRESHOLD"
    inputs: Dict[str, TypeSpec]   # {"price": TimeSeries[float], "period": int}
    outputs: Dict[str, TypeSpec]  # {"value": TimeSeries[float]}
    params: Dict[str, Any]        # {"period": 20}
```

**图连接推断**:
- 节点 A 输出 `value: TimeSeries[float]`
- 节点 B 输入 `price: TimeSeries[float]`
- 类型匹配 → 自动连接 A.value → B.price
- 同类型多输出时，用显式名称匹配

**编译产出 (IR)**:
- 选项 A: Protobuf 图拓扑描述
  ```protobuf
  message StrategyGraph {
    repeated NodeDef nodes = 1;
    repeated EdgeDef edges = 2;    // 自动推断 + 显式覆盖
    repeated ParamBinding params = 3;
  }
  ```
- 选项 B: FlatBuffers（零拷贝反序列化，性能更优）
- 选项 C: JSON/YAML（开发调试方便，性能次优）
- 选项 D: 直接生成 C++ 代码 → 编译为 .so → dlopen 加载

**C++ 后端加载**:
```cpp
// 加载 IR → 构造 FactorDAG
auto graph = StrategyGraph::load_from_file("ma_cross.graph");
auto dag = FactorDAG::from_graph(graph);  // 根据 NodeDef + EdgeDef 构图
dag.compute(store, time_range);            // 执行
```

### 6.4 与现有代码的关系

| 现有组件 | 改造方向 |
|----------|----------|
| `FactorDAG` (C++) | 保留，增加 `from_graph(IR)` 工厂方法 |
| `FactorRegistry` (C++) | 保留，节点 op_type → 注册表查找构造函数 |
| `dsl.py` (Python) | 重写，增加 typed input/output 定义 |
| `compiler.py` (Python) | 重写，输出 IR 而非直接注册 C++ 对象 |
| `PythonExecutor` | 废弃，C++ 后端直接执行 |
| `EventBusBridge` | 废弃，C++ EventBus 直接处理 |
| `StrategyCompiler` | 重写为 IR 编译器 |

### 6.5 待完成

- [ ] 调研 Protobuf vs FlatBuffers vs JSON 作为 IR 格式
- [ ] 设计完整的 IR schema (NodeDef, EdgeDef, TypeSpec)
- [ ] 设计类型推断和连接匹配算法
- [ ] 实现 Python DSL v2 (typed nodes)
- [ ] 实现 Python → IR 编译器
- [ ] 实现 C++ IR 加载器 + FactorDAG::from_graph()
- [ ] 集成测试：Python 编写策略 → 编译 → C++ 加载执行

---

## 7. 任务汇总

| # | 任务 | 优先级 | 状态 |
|---|------|--------|------|
| 142 | Fix StorageEngine store_kline/query_kline codec mismatch | P0 | 待实施 |
| 143 | Strategy registration center (SQLite + CRUD + hot reload) | P1 | 待设计 |
| 144 | C++ resident service process | P0 | 待实施 |
| 145 | Complete DataIngestor network coroutines | P1 | 待实施 |
| 146 | Frontend strategy selection + backtest + results display | P2 | 待设计 |
| 147 | Strategy submission/registration API | P1 | 待设计 |
| — | Python DSL → IR → C++ 执行架构重设计 | P0 | 待出方案 |

---

## 8. 价格处理约定

- C++ 内部使用整数价格（1/10000 元），避免浮点精度问题
- Python 层使用 float（元为单位）
- pybind11 边界做转换：C++ int64 → Python float (÷10000)
- StorageEngine 存储用 int64，查询时按需转换
