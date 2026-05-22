# QuantInvest — A股量化投资系统

人类主导、AI 辅助的量化投资系统，AI 极大放大了专家的能力边界与产出效率，但系统的上限始终由人来决定——如同带领一个团队。docs 目录下集中了设计文档，并有一部分多轮人-AI 交互讨论记录，指导并约束 AI 架构与编码。

## 系统核心特点

### 全协程无阻塞架构

全系统基于 folly::coro 构建，所有任务以协程方式调度，**禁止阻塞操作**（禁止 `std::mutex`/`std::shared_mutex`/`std::condition_variable`/`std::this_thread::sleep_for`），确保 CPU 核充分并行利用。

- **WorkStealingExecutor**: 工作窃取调度器，每 worker 线程维护本地双端队列（ChaseLevDeque），协程唤醒时路由到原始线程保持 CPU 缓存局部性
- **AffinityBaton / AffinityMutex / AffinitySharedMutex**: 自研协程友好同步原语，单原子状态字 + 侵入式等待者链表，唤醒经 `add_to_worker()` 保持线程亲和

> `folly::coro::Mutex`/`SharedMutex` 不满足线程亲和要求（唤醒不经过 executor 路由，SharedMutex 内部含 SpinLock），故自研 Affinity 系列原语。未来规划全用户态化（SPDK 本地盘 IO、DPDK/RDMA 网络 IO），构建完整 RTC (Run-To-Complete) 在线任务模型。

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

> **环境**: Ubuntu 24.04, Linux 6.17, x86_64 | AMD Ryzen 9 7950X (16C/32T, L3 64MB) | 96 GB DDR5 | GCC 14.2

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
