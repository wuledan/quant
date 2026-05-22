# 引擎架构设计

> 日期: 2026-05-22
> 状态: Active Development

---

## 1. 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│  Python 策略层                                                    │
│  DSL v2 定义 → IRCompiler 编译 → IR JSON → etcd 提交              │
└──────────────────────────┬───────────────────────────────────────┘
                           │ etcd watch
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│  C++ 执行引擎                                                    │
│                                                                  │
│  ┌───────────────┐  ┌──────────────┐  ┌───────────────────────┐ │
│  │ StrategyEngine │  │ FactorDAG    │  │ BacktestRunner        │ │
│  │ 策略注册/激活   │  │ 因子依赖图    │  │ 回测执行              │ │
│  └───────┬───────┘  └──────┬───────┘  └───────────┬───────────┘ │
│          │                 │                      │             │
│          └────────┬────────┘──────────────────────┘             │
│                   ▼                                              │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 多级存储引擎                                                │ │
│  │  Tier 2: LRU Cache (内存热数据, AffinitySharedMutex)        │ │
│  │  Tier 1: Columnar Segments (磁盘温数据, SegmentIndex)       │ │
│  │  Tier 0: MinIO/etcd/PostgreSQL (远端冷数据)                 │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 基础设施层                                                  │ │
│  │  WorkStealingExecutor (线程亲和调度)                         │ │
│  │  AffinityBaton / AffinityMutex / AffinitySharedMutex       │ │
│  │  EventBus / EtcdClient / StrategyWatcher                   │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
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

```cpp
class StrategyEngine {
public:
    // 策略注册表（全局单例）
    static Registry& registry();
    
    // 查找策略
    Strategy* find_by_name(const std::string& name);
    
    // 注册/注销
    void register_strategy(std::unique_ptr<Strategy> strategy);
    void remove_strategy(const std::string& name);
    
    // 激活/停用
    void activate(const std::string& name);
    void deactivate(const std::string& name);
};
```

### 2.2 StrategyGraph + FactorDAG

策略编译的产物是一个**有向无环图（DAG）**，节点是因子计算，边是数据依赖。

```cpp
class StrategyGraph {
public:
    // 从 IR JSON 构建
    static std::unique_ptr<StrategyGraph> load_from_json(const std::string& ir_json);
    
    // 因子节点
    struct FactorNode {
        std::string name;        // 因子名，如 "ma_5", "ma_20"
        std::string op;          // 操作，如 "sma", "cross_above"
        std::vector<int> inputs; // 输入节点索引
    };
    
    // 拓扑排序执行
    void execute(ExecutionContext& ctx);
};
```

**FactorDAG 执行模型**:
- 拓扑排序确定执行顺序
- 同层无依赖的节点可并行执行
- 底层节点读取行情数据，中间节点计算技术指标，顶层节点生成交易信号

### 2.3 IR 编译流程

Python 端定义策略，编译为 IR JSON：

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

IR JSON 结构：
```json
{
  "nodes": [
    {"id": 0, "op": "sma", "params": {"period": 5}, "field": "close"},
    {"id": 1, "op": "sma", "params": {"period": 20}, "field": "close"},
    {"id": 2, "op": "cross_above", "inputs": [0, 1]}
  ],
  "outputs": [2]
}
```

---

## 3. 多级存储引擎

### 3.1 三级存储架构

| 层级 | 存储 | 数据温度 | 访问延迟 | 容量 |
|------|------|---------|---------|------|
| Tier 2 | LRU Cache (内存) | 热 | ~ns | 512 MB 可配置 |
| Tier 1 | Columnar Segment (磁盘) | 温 | ~ms | 本地磁盘 |
| Tier 0 | MinIO/etcd/PostgreSQL (远端) | 冷 | ~100ms | 无限 |

### 3.2 TimeSeriesCache

64 分片 LRU 缓存，每分片独立锁，支持协程友好读写。

```cpp
class TimeSeriesCache {
    // 协程 API（主路径）
    CoTask<void> co_append(symbol, data_type, row, source);
    CoTask<vector<KlineRow>> co_query(symbol, data_type, start_ts, end_ts);
    
    // 同步 API（向后兼容，内部 blockingWait）
    void append(symbol, data_type, row, source);
    vector<KlineRow> query(symbol, data_type, start_ts, end_ts);
};
```

**数据源追踪**: 每个缓存条目标记 `DataSource`：
- `kRealtimeIngest` — 引擎实时拉取，淘汰时需 flush 到下一层
- `kRemoteLoad` — 从远端加载，淘汰时直接丢弃
- `kUnknown` — 未标记来源

### 3.3 WriteBuffer + WAL

写入缓冲 + 预写日志，保证崩溃安全和写入效率。

```
单行 store_kline()
  → WAL 追加 (fsync, 崩溃恢复)
  → WriteBuffer 攒批
  → [8192 行 或 5s] 触发 flush
  → 批量写入 LRU Cache
  → 异步磁盘写入队列
```

WriteBuffer 使用 `AffinityMutex` + `AsyncScope` 协程后台 flush，无阻塞原语。

### 3.4 SegmentIndex

内存索引，消除 O(n) 目录遍历。启动时扫描 `.seg` 文件构建，查询 O(log n) 二分查找。

```cpp
class SegmentIndex {
    // Key: (symbol, data_type) → per-field sorted vector<SegmentMeta>
    CoTask<vector<SegmentMeta>> co_query(symbol, data_type, field, range);
    void add_segment(symbol, data_type, SegmentMeta);
    void remove_file(file_path);
};
```

### 3.5 数据生命周期

**核心原则**: 淘汰方向与同步方向正交。

| 数据来源 | 内存→磁盘 | 磁盘→MinIO | 说明 |
|---------|----------|-----------|------|
| 实时拉取 | 先 flush 再淘汰 | 先上传再删除 | 沉淀 |
| 远端加载 | 直接淘汰 | 直接删除 | 纯淘汰 |

---

## 4. 基础设施层

### 4.1 协程调度 — WorkStealingExecutor

基于 folly::coro 的工作窃取调度器，线程亲和设计。

```
┌─────────────────────────────────────────────────┐
│  WorkStealingExecutor                            │
│                                                  │
│  Worker 0 ─── local_queue ─── [coro1, coro2]     │
│  Worker 1 ─── local_queue ─── [coro3]            │
│  Worker 2 ─── local_queue ─── []                 │
│  Worker 3 ─── local_queue ─── [coro4, coro5]     │
│                                                  │
│  global_queue ──── [coro6, coro7]                 │
│                                                  │
│  空闲 Worker 从 global_queue 或其他 Worker 窃取    │
│  协程唤醒时路由到原始 Worker（线程亲和）             │
└─────────────────────────────────────────────────┘
```

**关键特性**:
- 每个 worker 线程维护本地双端队列（ChaseLevDeque）
- 空闲 worker 从全局队列或其他 worker 窃取任务
- 协程挂起时记录 `worker_id`，唤醒时通过 `add_to_worker(worker_id, handle)` 路由到原始线程
- 保持 CPU 缓存局部性，避免跨线程调度伪共享

### 4.2 Affinity 系列同步原语

自研协程友好同步原语，核心约束：**唤醒必须路由到原始 worker 线程**。

> **设计决策**: `folly::coro::Mutex` 和 `folly::coro::SharedMutex` 不满足线程亲和性要求
> （唤醒不经过 executor 路由，SharedMutex 内部使用 SpinLock），因此自研。

#### AffinityBaton

单次通知原语，用于协程间信号传递。

```
waiter 协程:  co_await baton.co_wait()  → 挂起，记录 worker_id
notifier:     baton.post()              → 通过 add_to_worker() 路由唤醒
```

#### AffinityMutex

互斥锁，单原子状态字编码锁标志 + 等待者指针。

```
state_ 编码:
  0           → 未锁定
  kLockedFlag → 已锁定，无等待者
  ptr|flag    → 已锁定，ptr 指向等待者链表

co_lock():    尝试 CAS 设置 kLockedFlag → 失败则入队等待 → 挂起
unlock():     无等待者 → CAS 清除 → 有等待者 → 出队 → add_to_worker() 唤醒
```

#### AffinitySharedMutex

读写锁，支持并发读 / 独占写，写饥饿预防。

```
state_ 编码 (uint32_t):
  bit 0: kWriterLocked   — 写锁持有
  bit 1: kWriterWaiting  — 写者等待（阻止新读者）
  bit 2+: reader_count   — 活跃读者数

co_lock_shared():          reader_count++ (if no writer)
co_lock():                 设置 kWriterLocked
写饥饿预防:                 kWriterWaiting 时新读者排队
unlock():                  唤醒所有连续读者 或 下一个写者
```

### 4.3 EtcdClient + StrategyWatcher

**EtcdClient**: 基于 etcdctl 子进程的 etcd 交互客户端。

```
get/put/remove/get_prefix → popen("etcdctl ...") → 解析输出
co_watch_prefix           → 后台 jthread + popen("etcdctl watch")
                           → UnboundedQueue 传递事件到协程世界
```

> **临时方案**: etcd-cpp-apiv3 的 gRPC 在 FetchContent 构建下有兼容性问题。
> etcdctl 子进程可靠但延迟较高 (~10-50ms)，策略配置路径可接受。
> 后续替换为直接 gRPC 调用。

**StrategyWatcher**: 监控 etcd 策略变更，驱动引擎加载/卸载。

```
启动: get_prefix("/quant/strategy/") → 加载所有已有策略
监听: co_watch_prefix("/quant/strategy/") 
  → PUT /quant/strategy/{id}/ir   → load_from_json → register → activate
  → DELETE /quant/strategy/{id}/* → deactivate → remove
监听: co_watch_prefix("/quant/backtest/task/")
  → PUT → 触发回测执行
```

### 4.4 EventBus

进程内事件总线，发布/订阅模式，支持协程异步发布。

```cpp
class EventBus {
    // 同步发布
    void publish(event);
    // 协程异步发布（不阻塞调用方）
    CoTask<void> co_publish_async(event);
    // 订阅
    Subscription subscribe(event_type, callback);
};
```

### 4.5 WsEventBridge

EventBus → WebSocket 桥接，将引擎事件推送至前端。

```
EventBus.publish(kline_event)
  → WsEventBridge.on_event()
  → ws_server.broadcast(json_message)
  → 前端 WebSocket 实时更新
```

---

## 5. 交互设计

### 5.1 策略提交流

```
Python 端                          C++ 引擎
─────────                          ─────────
MACross 定义
  │
  ├─ IRCompiler.compile()
  │    → IR JSON
  │
  ├─ etcd_client.put(
  │    "/quant/strategy/42/ir",
  │    ir_json)
  │                          ───→  StrategyWatcher 收到 PUT 事件
  │                                  │
  │                                  ├─ StrategyGraph::load_from_json()
  │                                  ├─ FactorDAG 构建
  │                                  └─ StrategyEngine::register + activate
  │
  └─ 返回 strategy_id=42
```

### 5.2 行情数据流

```
DataIngestor (TCP/WS 行情源)
  │
  ├─ KlineRow
  │    ├─ WAL 追加 (fsync, 崩溃安全)
  │    └─ WriteBuffer 攒批
  │
  ├─ [8192行 / 5s] flush
  │    └─ LRU Cache append (source=kRealtimeIngest)
  │
  ├─ FactorDAG 实时计算
  │    ├─ 因子值 → LRU Cache
  │    ├─ 信号检测 → OrderSignalHandler
  │    └─ EventBus → WsEventBridge → 前端
  │
  └─ 逐步沉淀
       ├─ [30s] 缓存脏块 → .seg 磁盘文件
       └─ [每天] 冷段 → Parquet → MinIO
```

### 5.3 回测执行流

```
Python: etcd_client.put("/quant/backtest/task/1", task_json)
  │
  └──→ StrategyWatcher 收到 PUT
         │
         ├─ 拉取策略 IR
         ├─ BacktestRunner.run(strategy, symbol, [start, end])
         │    │
         │    ├─ query_kline() → Cache → Disk → MinIO
         │    ├─ FactorDAG.execute() — 逐 bar 驱动
         │    ├─ NAV 曲线计算
         │    └─ 回测指标 (sharpe, max_drawdown, ...)
         │
         └─ 结果写入 etcd / PostgreSQL
```

---

## 6. 部署架构

```
Docker Compose (deploy/)
├── etcd        — 策略中心 + 回测任务队列
│                 端口: 2379/2380
├── MinIO       — 行情/因子归档 (Parquet)
│                 端口: 9010 (API) / 9011 (Console)
└── minio-init  — 初始化 bucket: quant-kline, quant-factor, quant-corporate-actions

C++ 引擎 — 单进程，多协程
├── WorkStealingExecutor (4+ worker 线程)
├── StrategyWatcher (etcd watch 后台线程)
└── WriteBuffer (协程后台 flush)
```

---

## 7. 设计决策记录

| 决策 | 理由 | 日期 |
|------|------|------|
| 自研 AffinityMutex/SharedMutex 替代 folly::coro::Mutex | folly 版本无线程亲和性，SharedMutex 内部用 SpinLock | 2026-05-22 |
| etcdctl 子进程替代 etcd-cpp-apiv3 | gRPC FetchContent 构建兼容性问题 | 2026-05-22 |
| 移除 mmap 零拷贝读盘 | 热数据在缓存，冷数据瓶颈是解压不是 IO | 2026-05-22 |
| 淘汰方向与同步方向正交 | 实时数据淘汰需同步，远端数据淘汰无需回写 | 2026-05-22 |
