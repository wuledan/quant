# QuantInvest — A股量化投资系统

人类主导、AI 辅助的量化投资系统，AI 极大放大了专家的能力边界与产出效率，但系统的上限，目前还是由人来决定——如同带领一个团队。不过，随着系统不断的进化，人类专家的经验能力和决策能力，总有一天会被替代。
docs 目录下集中了设计文档，并有一部分多轮人-AI 交互讨论记录，指导并约束 AI 架构与编码。

## 系统核心特点

### 全协程无阻塞架构

全系统基于 folly::coro 构建，所有任务以协程方式调度，**禁止阻塞操作**（禁止 `std::mutex`/`std::shared_mutex`/`std::condition_variable`/`std::this_thread::sleep_for`），确保 CPU 核充分并行利用。

#### 为什么选择 Work-Stealing

量化计算具有典型的**非均匀并行**特征：因子 DAG 中不同节点的计算量差异大（SMA 是 O(n) 滑动窗口，CROSS_ABOVE 是 O(1) 比较），如果使用传统的线程池固定分配，长任务会阻塞短任务，导致尾部延迟放大。

Work-Stealing 天然适合这种场景：空闲 worker 主动从繁忙 worker 的队列尾部窃取任务，实现**动态负载均衡**，无需中心调度器介入。

```
Worker 0: [SMA_5 ████████████████░░░░]  ← 繁忙
Worker 1: [RSI_14  ████░░░░░░░░░░░░░░]
Worker 2: [idle]  ──steal──→ SMA_5 的后半段
Worker 3: [EMA_10 █████░░░░░░░░░░░░░░]
```

#### 线程亲和性：为什么唤醒必须路由到原线程

协程挂起后，其**工作数据集（working set）仍留在原 CPU core 的 L1/L2 缓存中**。如果唤醒时被调度到其他 core，所有数据需要从远端缓存或内存重新加载——这就是**缓存迁移（cache migration）**的代价。

量化计算场景中一次因子求值的数据集小（close_prices 数组 span），在 L1 缓存中热命中。跨 core 唤醒意味着：
- L1 命中 → L3 未命中（~40 cycles → ~200 cycles）
- NUMA 远端内存访问（~300 cycles, 5-7x 延迟）

`WorkStealingExecutor` 的 `add_to_worker(worker_id, handle)` 机制确保协程唤醒时精确恢复到挂起前的 CPU core，保持缓存热状态。32 个 worker 各绑一个物理 core（hwloc CPU pinning），同 NUMA 节点内窃取，跨 NUMA 隔离，最大化数据局部性。

#### NUMA 感知调度

本机 2 个 NUMA 节点（Intel Xeon Platinum 8173M 双路），每个节点有独立的本地内存控制器。跨 NUMA 访问远端内存的延迟是本地内存的 1.5-2x，且带宽受 QPI/UPI 链路限制。

```
NUMA Node 0 (31 GB)              NUMA Node 1 (63 GB)
├── Core 0-27 (56 HT)            ├── Core 28-55 (56 HT)
├── Worker 0-15                  ├── Worker 16-31
├── Cache Shard 0-7              ├── Cache Shard 8-15
└── .seg files on local disk     └── .seg files on local disk
```

- **Worker 绑核**：每个 worker 通过 hwloc 绑定到指定物理 core（跳过 HT sibling），避免两个 worker 争抢同一 L1/L2 缓存
- **同 NUMA 窃取**：work-stealing 仅在 `numa_peers_[my_numa]` 范围内选 victim，杜绝跨节点窃取造成的远端内存访问
- **Per-NUMA 全局队列**：外部任务提交按目标 worker 的 NUMA 路由到对应队列，唤醒仅通知同节点 worker

这保证了因子计算的数据（`span<const float>`）始终在本地内存，写入的因子结果也留在本地缓存——一次量化任务的全生命周期不离开其 NUMA 节点。

#### 自研 Affinity 系列同步原语

`folly::coro::Mutex`/`SharedMutex` 不满足线程亲和要求（唤醒不经过 executor 路由，SharedMutex 内部含 SpinLock），因此自研：

- **AffinityBaton** — 单次通知原语，post 时通过 `add_to_worker()` 路由唤醒
- **AffinityMutex** — 互斥锁，单原子状态字编码锁标志+等待者指针，CAS 快速路径
- **AffinitySharedMutex** — 读写锁，读者计数+写者标志+写饥饿预防

三种原语均由单原子操作实现快速路径（无竞争时零系统调用），唤醒路径保证线程亲和。


### Python DSL → IR 编译 → C++ 图执行

策略定义与执行完全解耦，形成完整的编译生效通路：

```
Python DSL v2 → IRCompiler → IR JSON → etcd 提交 → C++ 引擎 watch → FactorDAG → 执行
```

- Python 负责策略编写与 IR 编译，不参与运行时计算
- C++ 引擎 watch etcd 实时感知策略变更，自动加载/激活/卸载
- FactorDAG 拓扑排序执行，同层无依赖节点并行计算
- 回测任务通过 etcd 下发，引擎执行后写回结果

### 多级存储与数据生命周期

三级存储架构，数据从热层向冷层自动沉淀：

| 层级 | 存储 | 延迟 | 容量 |
|------|------|------|------|
| Tier 2 内存 | LRU Cache (分片, AffinitySharedMutex) | ~ns | 512 MB 可配置 |
| Tier 1 磁盘 | Columnar Segment Files (.seg, SegmentIndex) | ~ms | 本地磁盘 |
| Tier 0 远端 | etcd (策略) / MinIO (行情 Parquet) / PostgreSQL (元数据) | ~100ms | 无限 |

核心原则：**淘汰方向与同步方向正交** — 实时数据淘汰=沉淀（先 flush 到下一层），远端加载数据淘汰=纯淘汰（远端已有副本）。通过 `DataSource` enum 标记每个缓存条目的来源，驱动正确的淘汰策略。

数据规模支撑：5000+ A 股标的 × 10 年 × 6 频率 K 线 ≈ 204 GB 压缩后（Parquet Zstd ~7x），500 因子横截面 ≈ 25 GB。

### 高性能存储引擎

- **列式压缩**: ColumnBlock (Delta-of-Delta / Gorilla / Zstd)，每块 8192 行，压缩比 5:1~10:1
- **WAL + WriteBuffer**: 单行写入先追加 WAL（fsync 崩溃安全）→ 攒批 8192 行后批量压缩 → 写入缓存与磁盘
- **SegmentIndex**: 内存索引消除 O(n) 目录遍历，查询 O(log n) 二分查找
- **io_uring 异步 I/O**: `co_write_segment()`/`co_read_segment()` 绕过 page cache，避免阻塞
- **Read-Through**: 查询 Cache → SegmentIndex → 磁盘 → 回填缓存，自动从下层加载

### 事件驱动总线

EventBus 发布/订阅模式，同步派发延迟 < 1μs（热路径零分配），异步派发 500 万事件/秒。KlineEvent → FactorUpdateEvent → TradeSignalEvent → RiskAlertEvent 形成完整数据处理流水线。

## 架构概览

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
│  │ (策略管理)    │  │ (因子依赖图) │  │ (回测执行)           │ │
│  └─────────────┘  └──────────────┘  └─────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 多级存储引擎                                            │ │
│  │  Tier 2: LRU Cache (内存热数据, 分片 64, AffinitySharedMutex)│
│  │  Tier 1: Columnar Segment Files (磁盘温数据, SegmentIndex)│
│  │  Tier 0: MinIO/etcd/PostgreSQL (远端冷数据)              │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 协程调度器                                              │ │
│  │  WorkStealingExecutor (线程亲和)                         │ │
│  │  AffinityBaton / AffinityMutex / AffinitySharedMutex    │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

## 项目结构

```
├── cpp/quant/
│   ├── infra/          # 协程调度、同步原语、配置、etcd 客户端
│   ├── storage/        # 多级存储引擎（缓存/WAL/WriteBuffer/Segment/SegmentIndex）
│   ├── service/        # 常驻服务入口
│   ├── strategy/       # 策略引擎、FactorDAG、IR 加载、信号处理
│   ├── backtest/       # 回测框架
│   ├── network/        # TCP/WebSocket/HTTP、EventBus、CoIouring
│   ├── event/          # 事件类型定义、MPSC 无锁队列
│   └── pybind/         # Python 绑定
├── py/
│   ├── dsl/            # 策略 DSL 定义
│   ├── compiler/       # IR 编译器
│   └── client/         # 策略提交客户端
├── web/                # 前端 (React + WebSocket)
├── deploy/             # Docker 部署 (etcd + MinIO)
├── docs/               # 设计文档与讨论记录
└── Testing/            # 集成测试
```

## 性能基准

> **环境**: Ubuntu 24.04, Linux 6.17, x86_64 | Intel Xeon Platinum 8173M (2×28C/56T/112T, L3 77 MiB) | 96 GB DDR4 | GCC 14.2

| 组件 | 吞吐量 | 延迟 |
|------|--------|------|
| WorkStealingExecutor (4W, 100K tasks) | 409K ops/s | ~2.4 μs/task |
| DAG 中等规模 (1000 tasks × 10 层) | 648K ops/s | 1.5 ms |
| 空协程创建+执行 | 919K ops/s | ~1.09 μs |
| `co_await` 恢复 | — | ~242 ns |

详见 [benchmark_report.md](docs/benchmark_report.md)

## 快速开始

### 环境

- C++20 (GCC 12+ / Clang 15+), CMake 3.22+, Python 3.10+, Node.js 18+, Docker

### 构建与测试

```bash
cd build && cmake .. && make -j$(nproc)
ctest --output-on-failure
cd deploy && docker-compose up -d   # 启动 etcd + MinIO
cd web && npm install && npm run dev
```
