# 量化交易系统技术系列 — 从底层构建高性能 C++ 引擎

> 目标：从基础设施到底层存储、因子计算、策略编排，逐层构建一个完整的量化交易系统。
> 读者画像：有 C++ / 系统编程基础的技术读者，对量化交易感兴趣。

---

## 系列总览

```
第1篇  协程调度器
第2篇  协程同步原语
第3篇  NUMA 感知与 CPU 绑核
第4篇  列式存储引擎
第5篇  三级存储与数据生命周期
第6篇  因子 DAG 计算引擎
第7篇  事件总线
第8篇  DSL → IR → 执行：策略流水线
第9篇  系统集成：从行情推送到回测
```

---

## 第1篇：Work-Stealing 协程调度器

**副标题**: 为什么量化交易需要自己的调度器，而不是用线程池

### 1.1 问题引出
- 量化计算的并行特征：非均匀任务（SMA O(n) vs CROSS O(1)）
- 线程池固定分配的尾部延迟问题
- 为什么 Work-Stealing 天然适配

### 1.2 核心数据结构
- Chase-Lev Deque（双端队列）的 Lock-Free 实现
- `push()` / `pop()`（owner 端，LIFO）
- `steal()`（thief 端，FIFO）
- 内存序分析：为什么 steal 需要 acquire-release

### 1.3 Worker 循环
- 四级调度优先级：local deque → global queue → affine queue → steal
- 三级退避：PAUSE spin → sched_yield → futex park
- 防丢唤醒协议：parked flag + mutex 协同

### 1.4 线程亲和性
- 协程挂起时记录 worker_id
- 唤醒时路由到原线程：为什么 L1 缓存是核心资产
- 与 folly::coro 的对比：为什么自研

### 1.5 性能数据
- 409K ops/s (4 worker, 100K tasks)
- 空协程创建 ~1.09μs，恢复 ~242ns
- 对比 std::thread 和 asio::thread_pool

**关键代码**: `work_stealing_executor.h/.cc`, `chase_lev_deque.h`

---

## 第2篇：协程同步原语——AffinityBaton / Mutex / SharedMutex

**副标题**: 从原子操作到侵入式等待者链表，构建线程亲和的协程锁

### 2.1 为什么 folly::coro::Mutex 不够
- 唤醒不经过 executor 路由 → 缓存迁移
- SharedMutex 内部 SpinLock → 阻塞 worker 线程
- 实测对比数据

### 2.2 AffinityBaton：单次通知
- 原子状态字编码：posted flag + waiter pointer
- `await_ready()` / `await_suspend()` / `await_resume()` 三步协议
- `post()` 的唤醒路径：`add_to_worker(worker_id, handle)`

### 2.3 AffinityMutex：互斥锁
- 单原子 CAS 快速路径（无竞争零系统调用）
- 等待者链表：intrusive list + CAS 入队
- `unlock()` 出队 → executor 路由唤醒

### 2.4 AffinitySharedMutex：读写锁
- 状态字编码：writer locked + writer waiting + reader count
- 写饥饿预防：writer waiting 标志阻止新读者
- 批量读者唤醒：栈顶连续 reader 一次性全部恢复

### 2.5 调试验证
- 死锁检测
- 唤醒链追踪

**关键代码**: `affinity_baton.h/.cc`, `affinity_mutex.h/.cc`, `affinity_shared_mutex.h/.cc`

---

## 第3篇：NUMA 感知调度与 CPU 绑核

**副标题**: hwloc 拓扑发现 + 跨 NUMA 窃取隔离，最大化数据局部性

### 3.1 NUMA 架构回顾
- 双路 Xeon 拓扑：2 NUMA × 28 cores = 56 phys / 112 HT
- 远端内存访问代价：~300 cycles vs ~100 cycles 本地
- QPI/UPI 带宽限制

### 3.2 hwloc 拓扑发现
- `hwloc_topology_init/load` → 遍历 NUMA nodes
- 提取物理 core（跳过 HT siblings）
- 均匀分配 worker 到 NUMA 节点

### 3.3 CPU 绑核
- `hwloc_set_cpubind(THREAD)` 在线程启动时绑定
- 验证：`/proc/{pid}/task/{tid}/status` → `Cpus_allowed_list`

### 3.4 NUMA 局部窃取
- `numa_peers_[my_numa]` 替换全 worker 随机窃取
- 同节点任务不离开 L3 缓存
- 性能收益量化：跨 NUMA steal 率从 50% → 0%

### 3.5 Per-NUMA 全局队列
- 外部任务提交按 target_worker → NUMA 路由
- 局部唤醒：仅通知同节点 parked worker

**关键代码**: `work_stealing_executor.cc` (hwloc init + numa_peers_)

---

## 第4篇：列式存储引擎——ColumnBlock + Segment + WAL

**副标题**: Delta-of-Delta / Gorilla 压缩 + io_uring 异步 I/O 的时序列存

### 4.1 为什么列存
- K 线数据天然列式：timestamp, OHLCV 各列独立访问
- 压缩效率：同列数据规律性强
- 查询只取需要的列

### 4.2 ColumnBlock 压缩
- Delta-of-Delta：时间戳、成交量（int64）— 差值再差值，稀疏度极高
- Gorilla XOR：浮点价格 — XOR 前值相同的位，前导零 + 尾随零编码
- 块设计：8192 行/块，header + index + compressed data

### 4.3 Segment 文件格式
- `[Header 64B][BlockIndex × N][BlockData × N][Footer]`
- SegmentIndex 内存索引：O(log n) 二分查找
- Compaction：小段合并，降低碎片

### 4.4 WAL + WriteBuffer
- 单行写入 → WAL 追加（fsync 崩溃安全）
- 攒批 8192 行 → ColumnBlock 压缩 → 写入 cache + 磁盘
- WriteBuffer 协程化：`AffinityMutex + co_await sleep(5s)` 后台 flush

### 4.5 io_uring 异步 I/O
- `co_write_segment()` / `co_read_segment()` — 绕过 page cache
- fallback 路径：ring_ 为空时降级为 POSIX pread/pwrite

**关键代码**: `column_block.h/.cc`, `disk_persistence.h/.cc`, `write_buffer.h/.cc`, `write_ahead_log.h/.cc`

---

## 第5篇：三级存储与数据生命周期

**副标题**: LRU Cache → Columnar Segments → MinIO/PostgreSQL，数据自动沉淀

### 5.1 三级架构
- Tier 2: LRU Cache（分片 64, AffinitySharedMutex, 256MB）
- Tier 1: Columnar Segments（本地磁盘 .seg 文件, SegmentIndex）
- Tier 0: MinIO 远端 (kline Parquet) / PostgreSQL (回测结果) / etcd (策略 IR)

### 5.2 Read-Through 查询
- Cache hit → 直接返回
- Cache miss → SegmentIndex → 磁盘读取 → 解压 → 回填缓存
- 磁盘 miss → MinIO S3 下载 → 本地落盘 → 缓存

### 5.3 写入路径
- DataIngestor → WAL → WriteBuffer(攒批) → ColumnBlock 压缩 → Cache + 磁盘
- 数据源追踪：DataSource enum（kRealtimeIngest/kBatchLoad/kRemoteLoad）

### 5.4 淘汰策略
- LRU 按 last_access_ts 排序淘汰
- kRealtimeIngest：淘汰前沉淀到磁盘（数据不丢）
- kRemoteLoad：直接淘汰（远端已有副本）

### 5.5 ColdUpload Daemon
- 后台协程扫描 >30 天未访问的冷段
- 上传 MinIO → 本地标记 → 可选删除

**关键代码**: `time_series_cache.h/.cc`, `time_series_store.h/.cc`, `remote_storage.h/.cc`, `cold_upload_daemon.h/.cc`

---

## 第6篇：因子 DAG 计算引擎

**副标题**: 拓扑排序 + Wave 并行 + 内置因子（SMA/EMA/RSI/MACD/Bollinger）

### 6.1 DAG 结构
- 节点：因子（SMA_5, SMA_20, ...）
- 边：数据依赖（SMA_5.value → CROSS_ABOVE.fast）
- 拓扑排序 → Wave 并行

### 6.2 内置因子实现
- SMA：滑动窗口 O(n)，增量更新 O(1) per bar
- EMA：递推公式，无窗口限制
- RSI：Wilder 平滑平均增益/损失
- MACD：EMA_12 - EMA_26 的差值 + Signal line
- Bollinger：SMA ± k × σ

### 6.3 并行执行调度
- WaveScheduler::co_execute()：每个 Wave 内因子并行
- 协程数 = DAG 节点数
- 只读 span<const float>，无锁无竞争

### 6.4 FactorCache 复用
- Key: (trading_day, factor_id)
- 上层因子自动读取下层已缓存结果
- LRU 淘汰旧交易日数据

### 6.5 因子横截面存储（FactorStore）
- (date, symbol, factor_name, value) 四元组
- 支持截面排名、行业中性化、IC 检验

**关键代码**: `factor_dag.h/.cc`, `factor_computer.h/.cc`, `built_in_factors.h/.cc`, `factor_store.h/.cc`

---

## 第7篇：事件总线——低延迟发布/订阅

**副标题**: 1μs 同步派发 + 500 万事件/秒的协程化 EventBus

### 7.1 事件类型体系
- MarketDataEvent → KlineEvent → FactorUpdateEvent → TradeSignalEvent → OrderReportEvent → RiskAlertEvent
- 编译期类型安全：DEFINE_EVENT_TYPE 宏 + TypedEventBus 模板

### 7.2 同步派发热路径
- 零分配：不创建临时对象
- 读锁遍历订阅者列表（64 分片 AffinitySharedMutex）
- < 1μs 单订阅者

### 7.3 异步派发
- MPSC 无锁队列 → 批量取出 256-1024 事件 → 派发
- 协程化：AffinityBaton 替换 std::mutex + condition_variable
- 事件回放：Ring Buffer 无锁读取

### 7.4 阻塞原语迁移
- 13 个组件从 std::mutex / std::shared_mutex / std::condition_variable
  → AffinityMutex / AffinitySharedMutex / AffinityBaton
- 迁移策略：先低风险组件（WsSession/WebSocketServer）→ 再热路径（EventBus/ConfigManager）

**关键代码**: `event_bus.h/.cc`, `typed_event_bus.h`, `events/*.h`

---

## 第8篇：DSL → IR → 执行：策略编译流水线

**副标题**: Python 定义策略 → IR JSON 编译 → C++ 引擎 watch etcd → FactorDAG 激活

### 8.1 Python DSL
- StrategyBase 基类 + `build()` 方法
- 算子：sma/ema/cross_above/threshold
- IR 编译器：编译为 JSON → etcd 提交

### 8.2 IR 数据结构
- NodeDef: id, op_type, params, inputs/outputs
- EdgeDef: from_node/from_port → to_node/to_port
- DataBinding: data_source → to_node/to_port

### 8.3 C++ 引擎 Watch
- EtcdClient：etcdctl subprocess → `co_watch_prefix`
- StrategyWatcher：PUT 事件 → `load_from_json` → `register_strategy` → `activate`
- 多策略并发管理：AsyncScope

### 8.4 策略激活与执行
- FactorDAG::from_graph(IR) → 构建 DAG
- StrategyRunner：协程消费 KlineEvent → 跑 DAG → 产出信号
- 信号 → WebSocket → 前端实时展示

### 8.5 回测执行
- BacktestRunner：逐 bar 驱动 DAG → 模拟成交
- NAV 曲线 + trades 记录 + 夏普/最大回撤
- 结果写入 PostgreSQL / etcd

**关键代码**: `strategy_base.py`, `ir_compiler.py`, `ir_graph.h/.cc`, `strategy_engine.h/.cc`, `strategy_runner.h/.cc`, `backtest_runner.h/.cc`

---

## 第9篇：系统集成——从行情推送到前端看板

**副标题**: AKShare 实时桥接 → DataIngestor → 存储 → API → React 前端

### 9.1 数据管线
- 桥接：Python TCP Server → DataIngestor 协议（长度前缀 JSON）
- C++ DataIngestor：连接桥接 → 解析 KlineRow → WAL → WriteBuffer → Cache
- REST API：策略 CRUD + 回测 + 行情查询 + 因子计算

### 9.2 前端可视化
- lightweight-charts: 蜡烛图 + SMA 叠加线 + 买卖信号标记
- AutoComplete：代码/名称模糊匹配 700+ 标的
- WebSocket：实时信号推送

### 9.3 端到端时延
- 桥接拉取 → DataIngestor 解析 → WriteBuffer → Cache → EventBus → 前端渲染
- 实测端到端 < 100ms

### 9.4 Docker 部署
- docker-compose: etcd + MinIO
- C++ 单进程：32 worker 协程 + 703 标的数据

### 9.5 未来展望
- 多策略并发实盘
- Level 2 Tick 数据处理
- XTP 券商直连交易
- 回测参数自动优化

**关键代码**: `market_data_bridge.py`, `data_ingestor.h/.cc`, `strategy_api.h/.cc`, `MarketData.tsx`

---

## 附：每篇文章的技术亮点标签

| 篇 | 标签 |
|----|------|
| 1 | Lock-Free 数据结构, 协程, Work-Stealing, Chase-Lev, CAS, 内存序 |
| 2 | 原子操作, 侵入式链表, 读写锁, 写饥饿预防, 线程亲和 |
| 3 | NUMA, hwloc, CPU 绑核, 缓存局部性, 跨节点隔离 |
| 4 | 列式存储, Delta 编码, Gorilla, io_uring, WAL, Segment |
| 5 | 三级存储, Read-Through, LRU 淘汰, MinIO, 数据生命周期 |
| 6 | DAG, 拓扑排序, Wave 并行, 技术因子, 横截面 |
| 7 | 事件驱动, 发布/订阅, MPSC 队列, 零分配, 协程化 |
| 8 | DSL 编译, IR, etcd watch, 策略激活, 回测 |
| 9 | TCP 桥接, REST API, React 图表, WebSocket, 端到端 |
