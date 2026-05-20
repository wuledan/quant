# 架构重设计：C++ 常驻引擎 + Python 策略热加载

> 日期: 2026-05-20
> 状态: 设计阶段
> 前置文档: architecture_cpp_core.md, architecture_python.md

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

## 2. 目标架构

```
┌─────────────────────────────────────────────────────────┐
│                   C++ 后端服务 (常驻进程)                  │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │Storage   │  │EventBus  │  │Scheduler │  │WebSocket│ │
│  │Engine    │  │          │  │Service   │  │Server   │ │
│  │          │  │ Kline ──→│  │ cron+dag │  │ :8080   │ │
│  │ store()  │  │ Factor──→│  │          │  │         │ │
│  │ query()  │  │ Signal──→│  │          │  │ push    │ │
│  │ flush()  │  │ Order───→│  │          │  │         │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬────┘ │
│       │              │              │              │      │
│       └──────────────┴──────────────┴──────────────┘      │
│                        pybind11                           │
└────────────────────────┬────────────────────────────────┘
                         │ import _quant_core
┌────────────────────────┴────────────────────────────────┐
│                  Python 策略层 (热加载)                    │
│                                                          │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │策略脚本   │  │策略编译器 │  │回测/交易  │              │
│  │.py 文件  │  │→ DAG 图  │  │runner    │              │
│  └──────────┘  └──────────┘  └──────────┘              │
│                                                          │
│  策略文件修改 → 自动重载 → 编译为执行图 → 提交到C++引擎    │
└─────────────────────────────────────────────────────────┘
```

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

### 3.2 Python 策略热加载 + DAG 编译

**职责**: 策略快速迭代, 声明式定义, 编译为 C++ DAG

**策略开发流程**:
```
编辑 ma_cross.py → 保存 → 文件监视器检测变更 → 编译为执行图 → 注册到引擎
```

**策略声明式定义**:
```python
from quant_invest.strategy import Strategy, Factor, cross_above

@strategy("ma_cross")
class MACross:
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    signal = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx):
        if self.signal > 0:
            ctx.order(symbol=ctx.symbol, side=BUY, quantity=ctx.cash * 0.95 / ctx.price)
        elif self.signal < 0:
            ctx.order(symbol=ctx.symbol, side=SELL, quantity=ctx.position)
```

**编译结果**:
- 因子节点注册到 C++ `FactorRegistry` → `FactorDAG.build()` 自动拓扑排序
- 信号节点注册到 EventBus 订阅
- 策略修改只影响自己的 DAG 子图, 不影响其他策略

### 3.3 回测引擎 (走 StorageEngine + FactorDAG)

**职责**: 高性能回测, 数据零拷贝, 因子 C++ 并行计算

**数据流**:
```
StorageEngine.query_kline() → numpy array (零拷贝)
  → FactorDAG.compute() → 因子值 (C++ 并行)
  → Python strategy.on_signal() → 信号
  → C++ SimulatedBroker.execute() → 成交
  → Portfolio.update() → 净值
```

**对比当前**:
- 当前: Python 拉数据 → Python rolling → Python on_bar → Python broker
- 目标: C++ 读数据 → C++ 因子 → Python 信号 → C++ 订单

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
| 策略执行 | Python on_bar 循环 | Python 信号生成 + C++ 订单管理 |
| 策略迭代 | 改代码 → 重启 | 改代码 → 自动重载 → DAG 增量更新 |
| 回测数据 | 每次拉网/读 Parquet | 直接读 StorageEngine 零拷贝 |
| 实时推送 | FastAPI WS | C++ WebSocketServer io_uring |
| 进程模型 | Python 单进程 | C++ 常驻 + Python 策略热加载 |

## 5. 实施路径

### P0: C++ StorageEngine 接入数据采集
- 编译 pybind11 模块, 确保 `_quant_core` 可 import
- 实现 DataIngestor: C++ 协程定时拉取 → store_kline_batch()
- Python 调度器改为调用 C++ StorageEngine, 替代 Parquet 缓存
- 验证: query_kline 返回正确数据

### P1: Python 策略热加载 + DAG 编译器
- 实现文件监视器 (watchfiles)
- 策略声明式 DSL: Factor / cross_above / cross_below
- 策略编译器: Python AST → FactorDAG 节点注册
- 策略热重载: 检测变更 → 增量更新 DAG → 不影响运行中的策略
- 验证: 修改策略文件 → 自动生效, 无需重启

### P2: 回测走 StorageEngine + FactorDAG
- BacktestRunner: 从 StorageEngine 读数据 (零拷贝)
- 因子计算走 C++ FactorDAG (并行)
- 策略信号生成走 Python
- 订单模拟走 C++ SimulatedBroker
- 验证: 回测结果与当前 Python 引擎一致, 速度提升

### P3: C++ WebSocketServer 替代 FastAPI WS
- 启动 WebSocketServer :8080
- EventBus 订阅 → broadcast 到前端
- 前端连接切换到 :8080
- 验证: 实时数据推送延迟 < 10ms

### P4: 交易时段 EventBus 全链路打通
- KlineEvent → FactorDAG → FactorUpdateEvent → Strategy → SignalEvent → OrderManager
- 风控: RiskEngine 检查订单 → RiskAlertEvent
- 验证: 端到端延迟 < 1ms (同进程内)

## 6. Agent 团队分工

| Agent | 方向 | 负责组件 |
|-------|------|----------|
| storage-engine-expert | C++ 存储引擎 | P0: StorageEngine 接入, DataIngestor, pybind 绑定 |
| storage-engine-expert | C++ 存储引擎 | P3: WebSocketServer 接入, EventBus 推送 |
| go-backend-expert | 后端架构 | P1: 策略热加载框架, DAG 编译器, 文件监视器 |
| go-backend-expert | 后端架构 | P2: 回测 Runner 重构, StorageEngine 数据源 |
| backend-testing-expert | 后端测试 | 各组件单元测试, 集成测试, 性能基准测试 |
| 总架构师 | 架构协调 | 任务分配, Review, 推送, 架构文档更新 |

## 7. 验收标准

- [ ] C++ StorageEngine 可通过 pybind11 存取 K 线数据
- [ ] 数据采集写入 StorageEngine, 非 Python Parquet
- [ ] 策略文件修改后 5 秒内自动生效
- [ ] 回测从 StorageEngine 读数据, 无网络 IO
- [ ] 前端通过 C++ WebSocket 接收实时推送
- [ ] 交易时段全链路 EventBus 打通
- [ ] 所有组件有单元测试, 集成测试通过
- [ ] 架构文档更新, 记录变迁
