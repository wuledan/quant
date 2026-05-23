# 引擎架构设计

> 日期: 2026-05-22 | 更新: 2026-05-23
> 状态: Implemented — 全部目标已达成

---

## 1. 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│  Python 策略层                                               │
│  DSL v2 → IRCompiler → IR JSON → etcd 提交                   │
└──────────────────────┬───────────────────────────────────────┘
                       │ etcd watch
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  C++ 执行引擎                                                │
│                                                              │
│  ┌─────────────┐  ┌──────────────┐  ┌─────────────────────┐ │
│  │ StrategyEngine│  │ FactorDAG    │  │ BacktestRunner      │ │
│  │ 策略注册/激活 │  │ 因子依赖图   │  │ 回测执行            │ │
│  └───────┬───────┘  └──────┬───────┘  └───────────┬─────────┘ │
│          │                 │                      │           │
│          └────────┬────────┘──────────────────────┘           │
│                   ▼                                            │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 多级存储引擎                                            │ │
│  │  Tier 2: LRU Cache (内存热数据, AffinitySharedMutex)    │ │
│  │  Tier 1: Columnar Segments (磁盘温数据, SegmentIndex)   │ │
│  │  Tier 0: MinIO/etcd/PostgreSQL (远端冷数据)             │ │
│  │  Read-Through: Cache → SegmentIndex → Disk → MinIO      │ │
│  │  WAL + WriteBuffer (攒批 8192 行, 崩溃安全)            │ │
│  │  Compaction Daemon + ColdUpload Daemon                  │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 基础设施层                                              │ │
│  │  WorkStealingExecutor (线程亲和调度, 4+ worker)         │ │
│  │  AffinityBaton / AffinityMutex / AffinitySharedMutex    │ │
│  │  EventBus / EtcdClient / StrategyWatcher                │ │
│  │  CoIouring (io_uring 异步 I/O)                          │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

### 数据流

1. **策略流**: Python DSL → IRCompiler → IR JSON → etcd PUT → C++ StrategyWatcher watch → StrategyGraph::load_from_json() → FactorDAG → StrategyEngine 注册激活
2. **行情流**: DataIngestor 拉取 → KlineRow → WAL (crash safety) → WriteBuffer (攒批) → LRU Cache → 异步磁盘写入
3. **回测流**: etcd PUT /quant/backtest/task/ → StrategyWatcher → BacktestRunner → FactorDAG 计算 → 结果写回 etcd/PostgreSQL
4. **查询流**: BacktestRunner.query_kline() → LRU Cache hit? → SegmentIndex → 本地磁盘 → MinIO 拉取

---

## 2. 策略引擎

### 2.1 StrategyEngine

策略注册中心，管理所有活跃策略的生命周期。

**文件**: `cpp/quant/strategy/strategy_engine.h/.cc`
**测试**: 集成在 service_main_test, strategy_api_test 中通过

```cpp
class StrategyEngine {
public:
    StrategyEngine(event::EventBus& bus, storage::StorageEngine& storage);

    bool activate(uint64_t strategy_id);
    bool pause(uint64_t strategy_id);
    bool resume(uint64_t strategy_id);
    bool deactivate(uint64_t strategy_id);
    bool is_active(uint64_t strategy_id) const;

    StrategyRegistry& registry();
};
```

### 2.2 StrategyGraph + FactorDAG

策略编译的产物是有向无环图（DAG），节点是因子计算，边是数据依赖。

**文件**: `cpp/quant/ir/ir_graph.h/.cc`, `cpp/quant/factor/factor_dag.h/.cc`
**测试**: ir_graph_test, factor_dag_from_graph_test — 通过

```cpp
// IR 加载
auto graph = StrategyGraph::load_from_json(ir_json);

// 从 IR 构建 DAG
auto dag = FactorDAG::from_graph(graph, registry);

// 拓扑排序执行
auto levels = dag.parallel_levels();  // 按 Wave 并行
```

### 2.3 IR 编译流程

Python 端定义策略，编译为 IR JSON：

**文件**: `py/dsl/strategy_base.py`, `py/compiler/ir_compiler.py`, `py/client/etcd_submit.py`
**测试**: test_ir_compiler.py — 36/36 通过

```python
class MACross(StrategyBase):
    fast_period = 5
    slow_period = 20

    def build(self):
        ma_fast = sma("close", self.fast_period)
        ma_slow = sma("close", self.slow_period)
        signal = cross_above(ma_fast, ma_slow)
        return signal

# 编译 → 提交
ir_json = IRCompiler().compile(MACross)
etcd_client.put("/quant/strategy/42/ir", ir_json)
```

---

## 3. 多级存储引擎

### 3.1 三级存储架构 — 全部实现

| 层级 | 存储 | 实现文件 | 锁机制 | 状态 |
|------|------|---------|--------|------|
| Tier 2 内存 | LRU Cache (分片 16, 预算控制) | `time_series_cache.h/.cc` | AffinitySharedMutex | ✅ |
| Tier 1 磁盘 | Columnar Segment Files | `disk_persistence.h/.cc` | SegmentIndex (AffinitySharedMutex) | ✅ |
| Tier 0 远端 | MinIO/etcd/PostgreSQL | `remote_storage.h/.cc`, `postgres_store.h/.cc` | — | ✅ |

### 3.2 写入路径 — 完整实现

```
DataIngestor (TCP/WS 行情源)
  → KlineRow
  → WAL (write_ahead_log.h/.cc — fsync 崩溃安全)
  → WriteBuffer (write_buffer.h/.cc — AffinityMutex, 攒批 8192 行/5s)
  → CoTask flush → ColumnBlock 压缩 (Delta/Gorilla)
  → LRU Cache (time_series_cache.h/.cc — AffinitySharedMutex 分片)
  → 异步磁盘写入 (disk_persistence.h/.cc — io_uring co_write)
  → [每 30s] 缓存脏块 → .seg 文件 (storage_engine start_dirty_flush)
  → [每天] 冷段 → ColdUploadDaemon → RemoteStorage → MinIO
```

### 3.3 读取路径 — 完整实现

```
BacktestRunner.query_kline(symbol, data_type, range)
  → StorageEngine::query_kline → TimeSeriesCache::query (LRU hit)
  → [miss] → TimeSeriesStore::co_query_kline → SegmentIndex::co_query
  → DiskPersistence::co_read_segment (io_uring 异步读)
  → ColumnBlock::decompress → KlineRow 组装
  → 回填缓存 (DataSource::kRemoteLoad)
  → [最终 miss] → RemoteStorage::download_kline (MinIO S3 API)
  → 写入本地 .seg → 填充缓存 → 返回
```

### 3.4 列式压缩 — 完整实现

**文件**: `column_block.h/.cc`
**测试**: ColumnBlockTest 8/8 通过

| Codec | 用途 | 算法 |
|-------|------|------|
| `kDelta` | 时间戳、成交量 (int64)、价格 (int32×10000) | Delta-of-Delta |
| `kGorilla` | 浮点值 (double) | XOR-based 时序压缩 |
| `kNone` | 原始存储 | 无压缩 |

### 3.5 段索引与合并 — 完整实现

**文件**: `segment_index.h/.cc`, `disk_persistence.h/.cc`
**测试**: SegmentIndexTest + DiskPersistenceTest — 通过

- `SegmentIndex`: 启动时扫描 .seg 文件头构建内存索引，O(log n) 查询
- `DiskPersistence::compact()`: 合并小段 (<4096 行) 为大段
- `ColdUploadDaemon`: 后台协程扫描冷段 → 上传 MinIO → 可选删除本地

### 3.6 缓存淘汰 — 完整实现

**文件**: `time_series_cache.h/.cc`
**测试**: TimeSeriesCacheTest 10/10 通过

- LRU 淘汰策略，按 last_access_ts 升序淘汰
- DataSource enum 驱动淘汰规则:
  - `kRealtimeIngest`: 淘汰前需 flush 到磁盘（沉淀）
  - `kRemoteLoad`/`kBatchLoad`: 直接淘汰（纯淘汰）

---

## 4. 因子计算引擎

**文件**: `factor/built_in_factors.h/.cc`, `factor/factor_dag.h/.cc`, `factor/factor_registry.h/.cc`
**测试**: factor_test 22/22, factor_dag_from_graph_test — 通过

### 4.1 内置因子

| 因子 | 参数 | 实现 |
|------|------|------|
| SMA | period | `ma()` |
| EMA | period | `ema()` |
| RSI | period | `rsi()` |
| MACD | fast=12, slow=26, signal=9 | `macd()` → MACDResult |
| BOLL | period=20, num_std=2.0 | `bollinger()` → BollingerResult |

### 4.2 信号算子

| 算子 | 功能 |
|------|------|
| `CROSS_ABOVE` | 快线上穿慢线 |
| `CROSS_BELOW` | 快线下穿慢线 |
| `THRESHOLD` | 阈值判断 |
| `AND` / `OR` / `NOT` | 信号逻辑组合 |

### 4.3 DAG 执行

- 拓扑排序确定执行顺序
- 同层无依赖节点并行计算（Wave-based）
- SignalHandler 将因子信号转为 OrderSignal / RiskAlert

---

## 5. 事件系统

**文件**: `event/event_bus.h/.cc`, `event/events/*.h`
**测试**: event_bus_test 14/14, event_bus_co_test 3/3 — 通过

| 事件 | ID | 发布者 | 订阅者 |
|------|----|--------|--------|
| MarketDataEvent | 1 | DataIngestor | StorageEngine |
| KlineEvent | 2 | DataIngestor | StorageEngine, FactorComputer |
| TradeSignalEvent | 3 | SignalHandler | RiskEngine, OrderManager |
| OrderReportEvent | 4 | BrokerGateway | OrderManager, StrategyEngine |
| RiskAlertEvent | 5 | RiskEngine | AlertHandler, WsEventBridge |
| FactorUpdateEvent | 6 | FactorDAG | SignalHandler, StrategyEngine |

EventBus 已迁移为全协程同步原语（AffinitySharedMutex + AffinityBaton），线程派发改为协程 start_async() 路径。

---

## 6. 基础设施层

### 6.1 协程调度 — WorkStealingExecutor

**文件**: `infra/work_stealing_executor.h/.cc`
**测试**: work_stealing_executor_test — 通过

- 工作窃取调度器，每 worker 线程维护本地 ChaseLevDeque
- 协程唤醒时路由到原始 worker 线程（线程亲和）
- 三级退避: spin (PAUSE) → yield → park (futex)
- park_mutex+park_cv 标注为有意例外（OS 线程 parking）

### 6.2 Affinity 系列同步原语

**文件**: `infra/affinity_mutex.h/.cc`, `infra/affinity_shared_mutex.h/.cc`, `infra/affinity_baton.h/.cc`
**测试**: affinity_mutex_test, affinity_shared_mutex_test, affinity_baton_test — 通过

| 原语 | 用途 | 设计 |
|------|------|------|
| AffinityBaton | 协程间信号 | 单次通知，唤醒经 add_to_worker() |
| AffinityMutex | 互斥锁 | 单原子状态字 + 侵入式等待者链表 |
| AffinitySharedMutex | 读写锁 | 读者计数 + 写者标志 + 写饥饿预防 |

所有 13 个组件的 std::mutex/std::shared_mutex/std::condition_variable 已替换（T3.12 WorkStealingExecutor park 为有意例外）。

### 6.3 EtcdClient + StrategyWatcher

**文件**: `infra/etcd_client.h/.cc`, `infra/strategy_watcher.h/.cc`
**测试**: etcd_client_test, strategy_watcher_test 16/16 — 通过

- etcdctl 子进程方案（临时，后续可迁移到 gRPC）
- co_watch_prefix 使用后台线程 + UnboundedQueue 传递事件到协程世界
- StrategyWatcher 监听 `/quant/strategy/` 和 `/quant/backtest/task/` 前缀

### 6.4 io_uring 异步 I/O

**文件**: `network/co_io.h/.cc`, `network/global_io.h/.cc`
**测试**: co_io_test — 通过

- CoIouring: io_uring 协程 I/O (co_connect, co_send, co_recv, co_accept, co_read, co_write)
- DiskPersistence 的 co_write_segment/co_read_segment 使用 io_uring 路径

---

## 7. 阻塞原语迁移状态

| 组件 | 原原语 | 替换为 | 状态 |
|------|--------|--------|------|
| EventBus Shard | std::shared_mutex | AffinitySharedMutex | ✅ |
| EventBus async | std::mutex + CV | AffinityBaton | ✅ |
| ConfigManager | std::shared_mutex | AffinitySharedMutex | ✅ |
| FactorComputer | std::mutex | AffinityMutex | ✅ |
| CronScheduler | std::mutex + sleep_for | AffinityMutex + co_sleep | ✅ |
| WsSession | std::mutex | AffinityMutex | ✅ |
| WebSocketServer | std::mutex | AffinityMutex | ✅ |
| Logger | std::mutex+CV+shared_mutex | AffinityMutex+Baton+AffinitySharedMutex | ✅ |
| Metrics | std::shared_mutex | AffinitySharedMutex | ✅ |
| ObjectPool | std::mutex | AffinityMutex | ✅ |
| MemoryPool | std::mutex | AffinityMutex | ✅ |
| DataIngestor | std::mutex | AffinityMutex | ✅ |
| service_main | sleep_for | AffinityBaton | ✅ |
| WorkStealingExecutor | park_mutex+park_cv | **有意例外** (OS 线程停靠) | — |

---

## 8. 设计目标达成状态

| 目标 | 状态 | 实现 |
|------|------|------|
| 策略中心独立化 | ✅ | Python IR → etcd → C++ watch → FactorDAG |
| 数据规模支撑 (5000×10年×6频率) | ✅ | 三级存储 + 列式压缩 (~7x) |
| 多级存储自动流转 | ✅ | Cache→Disk→MinIO，Read-Through |
| 量化数据完备 | ✅ | K线 + 因子横截面 + 公司行为 + 复权 |
| 全协程无阻塞 | ✅ | 13/14 组件迁移 (1 有意例外) |
| Python DSL → IR → C++ | ✅ | ir_compiler.py → StrategyGraph → FactorDAG |
| WAL + WriteBuffer 崩溃安全 | ✅ | WAL fsync + 攒批 flush |
| io_uring 异步 I/O | ✅ | DiskPersistence + DataIngestor |
| etcd 策略变更实时生效 | ✅ | StrategyWatcher co_watch_prefix |
| 冷段自动归档 MinIO | ✅ | ColdUploadDaemon + RemoteStorage |

---

## 9. 测试覆盖

| 测试套件 | 测试数 | 状态 |
|----------|--------|------|
| ColumnBlock | 8 | ✅ |
| TimeSeriesCache | 10 | ✅ |
| SegmentIndex | 2 | ✅ |
| TimeSeriesStore | 11 | ✅ |
| StorageEngine | 4 | ✅ |
| WriteAheadLog | 2 | ✅ |
| DiskPersistence | 14 | ✅ |
| DataInit | 4 | ✅ |
| RemoteStorage | 9 (2 skipped) | ✅ |
| ColdUploadDaemon | 4 | ✅ |
| PostgresStore | 4 (3 skipped) | ✅ |
| FactorStore | 4 | ✅ |
| CorporateAction | 3 | ✅ |
| **C++ 合计** | **80 (75 pass, 5 skip)** | |
| Python IRCompiler | 36 | ✅ |
| EventBus | 17 | ✅ |
| Logger/Metrics | 27 | ✅ |
| etcd/Strategy | 16+ | ✅ |
