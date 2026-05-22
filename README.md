# QuantInvest — A股量化投资系统

基于 Python DSL → IR 编译 → C++ 执行的量化投资系统，支持多策略回测与实盘交易。

## 架构概览

```
┌──────────────────────────────────────────────────────────────┐
│  Python 策略层                                               │
│  DSL v2 → IRCompiler → IR JSON/Protobuf → etcd 提交          │
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
│  │  Tier 2: LRU Cache (内存热数据)                         │ │
│  │  Tier 1: Columnar Segment Files (磁盘温数据)            │ │
│  │  Tier 0: MinIO/etcd/PostgreSQL (远端冷数据)             │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 协程调度器                                              │ │
│  │  WorkStealingExecutor (线程亲和调度)                     │ │
│  │  AffinityBaton / AffinityMutex / AffinitySharedMutex    │ │
│  └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

## 核心设计特点

### 线程亲和协程模型

全系统基于 folly::coro 构建，所有任务以协程方式调度，**禁止阻塞操作**：

- **WorkStealingExecutor**: 工作窃取调度器，每个 worker 线程维护本地队列，协程唤醒时路由到原始 worker 线程
- **AffinityBaton**: 线程亲和的 baton，唤醒时通过 `add_to_worker()` 路由到正确线程
- **AffinityMutex**: 协程友好互斥锁，单原子状态字编码锁标志+等待者指针，唤醒保持线程亲和
- **AffinitySharedMutex**: 协程友好读写锁，支持并发读/独占写，写饥饿预防，唤醒保持线程亲和

> **设计决策**: `folly::coro::Mutex` 和 `folly::coro::SharedMutex` 不满足线程亲和性要求（唤醒不经过 executor 路由，SharedMutex 内部使用 SpinLock），因此自研了 Affinity 系列原语。

### Python → IR → C++ 策略流水线

策略定义与执行完全解耦：
1. Python 端定义策略（DSL v2），编译为 IR JSON
2. IR 提交至 etcd 策略中心
3. C++ 引擎 watch etcd，实时加载策略 → 构建 FactorDAG → 注册执行
4. 回测任务通过 etcd 提交，引擎执行并写回结果

### 多级存储与数据生命周期

三级存储架构（远端冷 → 磁盘温 → 内存热），核心原则：**淘汰方向与同步方向正交**。

| 数据来源 | 内存→磁盘 | 磁盘→MinIO | 说明 |
|---------|----------|-----------|------|
| 实时拉取 | 先 flush 再淘汰 | 先上传再删除 | 沉淀：淘汰=同步至下一层 |
| 远端加载 | 直接淘汰 | 直接删除 | 纯淘汰：远端已有，无需回写 |

通过 `DataSource` enum（`kRealtimeIngest`/`kRemoteLoad`）标记每个缓存条目的来源，驱动正确的淘汰策略。

## 项目结构

```
├── cpp/quant/
│   ├── infra/          # 协程调度、同步原语、配置
│   ├── storage/        # 多级存储引擎（缓存/WAL/WriteBuffer/Segment）
│   ├── service/        # 服务入口
│   ├── strategy/       # 策略引擎、FactorDAG、IR 加载
│   ├── backtest/       # 回测框架
│   ├── network/        # WebSocket、EventBus
│   ├── event/          # 事件定义、MPSC 队列
│   └── pybind/         # Python 绑定
├── py/
│   ├── dsl/            # 策略 DSL 定义
│   ├── compiler/       # IR 编译器
│   └── client/         # 策略提交客户端
├── web/                # 前端（React + WebSocket）
├── deploy/             # Docker 部署（etcd + MinIO）
├── docs/               # 设计文档与讨论记录
└── Testing/            # 集成测试
```

## 设计文档

| 文档 | 说明 |
|------|------|
| [多级存储架构设计](docs/multi_tier_storage_design.md) | 三级存储、数据生命周期、etcd/MinIO/PostgreSQL 选型与交互 |
| [策略 DSL 设计](docs/p1-t1-strategy-dsl-design.md) | Python DSL v2 → IR 编译 → C++ 执行的完整流程 |

## 快速开始

### 环境要求

- C++20 编译器 (GCC 12+ / Clang 15+)
- CMake 3.22+
- Python 3.10+
- Node.js 18+ (前端)
- Docker & Docker Compose (基础设施)

### 构建与测试

```bash
# 构建 C++ 引擎
cd build && cmake .. && make -j$(nproc)

# 运行测试
ctest --output-on-failure

# 启动基础设施
cd deploy && docker-compose up -d

# 启动前端
cd web && npm install && npm run dev
```

## License

Private — 内部项目
