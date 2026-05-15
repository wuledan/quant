# C++ 核心引擎架构设计

> 版本：v1.0 | 日期：2026-05-15 | 标准：C++20 | 编码规范：Google C++ Style

---

## 目录

1. [数据存储引擎](#1-数据存储引擎)
2. [因子计算引擎](#2-因子计算引擎)
3. [执行引擎](#3-执行引擎)
4. [风控引擎](#4-风控引擎)
5. [消息总线与事件系统](#5-消息总线与事件系统)
6. [跨模块集成](#6-跨模块集成)

---

## 1. 数据存储引擎

### 1.1 概述

时序数据存储引擎负责K线和Tick数据的持久化与高效查询，采用 **内存缓存 + 磁盘持久化** 双层架构，列式存储并支持压缩，针对范围查询和时序扫描进行深度优化。

### 1.2 核心接口定义

```cpp
// ===== 数据类型定义 =====

enum class DataType : uint8_t {
    kKline1Min  = 0,
    kKline5Min  = 1,
    kKline15Min = 2,
    kKline30Min = 3,
    kKline60Min = 4,
    kKlineDay   = 5,
    kTick       = 6,
};

enum class DataField : uint8_t {
    kOpen   = 0,
    kHigh   = 1,
    kLow    = 2,
    kClose  = 3,
    kVolume = 4,
    kAmount = 5,
    kVwap   = 6,
    // Tick fields
    kBidPrice1 = 10,
    kAskPrice1 = 11,
    kBidVol1   = 12,
    kAskVol1   = 13,
};

struct TimeRange {
    int64_t begin_ts;  // 含，微秒精度 Unix 时间戳
    int64_t end_ts;    // 含
};

// ===== 列式数据块（压缩单元） =====

class ColumnBlock {
public:
    // 压缩方式
    enum class Codec : uint8_t {
        kNone    = 0,
        kDelta   = 1,   // Delta-of-Delta（时序常用）
        kZstd    = 2,
        kLZ4     = 3,
        kGorilla = 4,   // 时序浮点专用
    };

    static constexpr size_t kBlockSize = 8192; // 每块最多 8192 行

    DataField field() const noexcept;
    Codec     codec() const noexcept;
    size_t    row_count() const noexcept;

    // 解压到目标缓冲区，返回实际行数
    size_t decompress(std::span<std::byte> dst) const;

    // 从原始数据压缩构造
    static ColumnBlock compress(DataField field,
                                std::span<const std::byte> src,
                                Codec codec);

private:
    // 内存布局：Header(32B) + compressed_data
    // Header: field(1) + codec(1) + row_count(2) + raw_size(4) + crc32(4) + reserved(20)
    alignas(64) std::byte storage_[];
};

// ===== 内存缓存层 =====

class TimeSeriesCache {
public:
    explicit TimeSeriesCache(size_t memory_budget_mb);

    // 写入：追加最新数据到缓存
    void append(std::string_view symbol,
                DataType type,
                std::span<const std::byte> row_data,
                size_t row_size);

    // 读取：优先从缓存返回，缺失部分由上层回源磁盘
    std::vector<ColumnBlock> query(std::string_view symbol,
                                    DataType type,
                                    DataField field,
                                    TimeRange range) const;

    // 淘汰：将冷数据刷写到磁盘
    void evict(std::string_view symbol, DataType type);

    size_t used_memory() const noexcept;

private:
    struct Shard {
        // symbol -> (DataType -> RingBuffer<ColumnBlock>)
        using ColumnMap = ankerl::unordered_dense::map<
            std::pair<std::string, DataType>,
            std::vector<ColumnBlock>,
            ankerl::unordered_dense::hash<std::pair<std::string, DataType>>>;

        std::shared_mutex rwlock;
        ColumnMap columns;
        size_t    memory_used = 0;
    };

    static constexpr size_t kShardCount = 64;
    std::array<Shard, kShardCount> shards_;
    std::atomic<size_t> total_memory_{0};
    size_t memory_budget_;
};

// ===== 磁盘持久化层 =====

class TimeSeriesStore {
public:
    struct Options {
        std::filesystem::path data_dir;
        size_t memory_cache_mb = 2048;     // 默认 2GB 缓存
        double   compaction_threshold = 0.5; // 碎片率超过则触发压缩
        uint32_t sync_interval_ms    = 100;  // fsync 间隔
    };

    explicit TimeSeriesStore(Options opts);

    // ===== 写入接口 =====

    // 批量写入K线数据
    void write_kline(std::string_view symbol,
                     DataType kline_type,
                     std::span<const KlineRow> rows);

    // 批量写入Tick数据
    void write_tick(std::string_view symbol,
                    std::span<const TickRow> rows);

    // ===== 查询接口 =====

    // 范围查询：返回结构化行数据
    template<typename RowType>
    std::vector<RowType> query(std::string_view symbol,
                               DataType type,
                               TimeRange range);

    // 列式范围查询：仅获取指定列，减少内存拷贝
    std::vector<ColumnBlock> query_columns(std::string_view symbol,
                                            DataType type,
                                            std::span<const DataField> fields,
                                            TimeRange range);

    // 最新N条数据查询
    template<typename RowType>
    std::vector<RowType> latest(std::string_view symbol,
                                 DataType type,
                                 size_t count);

    // ===== 管理接口 =====

    // 手动触发 compaction
    void compact(std::string_view symbol, DataType type);

    // 关闭：刷盘 + 写 manifest
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ===== K线/Tick 行结构 =====

#pragma pack(push, 1)
struct KlineRow {
    int64_t  timestamp;    // 微秒
    int32_t  open_price;   // 价格 × 10000（整型定点）
    int32_t  high_price;
    int32_t  low_price;
    int32_t  close_price;
    int64_t  volume;
    int64_t  amount;
    int32_t  vwap;
};
static_assert(sizeof(KlineRow) == 48);

struct TickRow {
    int64_t  timestamp;
    int32_t  last_price;
    int32_t  bid_price1;
    int32_t  ask_price1;
    int32_t  bid_vol1;
    int32_t  ask_vol1;
    int64_t  volume;
    int64_t  amount;
};
static_assert(sizeof(TickRow) == 40);
#pragma pack(pop)
```

### 1.3 数据流描述

```
                                    写入路径
                    ┌──────────────────────────────────────────┐
  行情源 ──TCP──>  写入队列 (MPSC) ──>  批量攒盘 ──>  内存缓存追加
  (Level2)                                                    │
                                                              ├─ 定时刷盘 (100ms)
                                                              v
                                                      磁盘 WAL + Segment 文件
                                                              │
                                                              └─ Compaction → 合并小段

                                    读取路径
                    ┌──────────────────────────────────────────┐
  查询请求 ──>  TimeSeriesCache::query()  ──┬─ 命中 → 返回
                                             └─ 缺失 → 回源磁盘层
                                                        │
                                              TimeSeriesStore::query_columns()
                                                        │
                                              ├─ 读 Segment 文件
                                              ├─ 解压 ColumnBlock
                                              └─ 填充缓存 + 返回
```

### 1.4 磁盘文件组织

```
data/
├── manifest.json                     # 元数据索引
├── symbols/
│   ├── 000001.SZ/
│   │   ├── kline_1min/
│   │   │   ├── seg_20260101_20260131.col  # 列式存储段
│   │   │   ├── seg_20260201_20260228.col
│   │   │   └── wal.log                             # 当前写入 WAL
│   │   ├── kline_day/
│   │   └── tick/
│   ├── 600000.SH/
│   │   └── ...
```

**Segment 文件内部格式（列式）**：
```
[FileHeader 128B]
  - magic, version, symbol, data_type, row_count, 
  - column_offsets[16], min_ts, max_ts, crc32

[ColumnSection × N]  (每列独立存储)
  [ColumnMeta 32B]   - field, codec, row_count, compressed_size, raw_size
  [ColumnData]        - 压缩后的列数据（Delta-of-Delta / Gorilla / Zstd）

[RowIndex]            - 每 256 行一个索引点，加速二分查找
[Footer 64B]         - 校验和、索引偏移
```

### 1.5 线程模型与并发策略

| 组件 | 线程模型 | 并发机制 |
|------|---------|---------|
| 写入队列 | 1 个写入线程 | MPSC 无锁队列，行情线程 push，写入线程 pop |
| 内存缓存 | 分片 64 把读写锁 | 读多写少：`shared_mutex`，读共享、写排他 |
| 磁盘写入 | 1 个刷盘线程 + 1 个 compaction 线程 | WAL 顺序写；compaction 独立线程不阻塞读写 |
| 查询路径 | 调用方线程（无阻塞） | 缓存命中无锁；缓存未MISS仅读写锁短暂持有 |

**关键设计**：
- 写入采用 **单线程顺序写** WAL，避免随机写；compaction 由独立线程完成，生成新 Segment 后原子替换
- 内存缓存采用 **分片锁**（64 分片），不同 symbol 的并发操作互不干扰
- 读取路径 **不分配堆内存**（返回 `span` 引用缓存内部数据），降低 GC 压力

### 1.6 性能目标

| 指标 | 目标值 |
|------|--------|
| K线范围查询延迟（1年数据） | < 5ms |
| Tick范围查询延迟（1天数据） | < 10ms |
| 写入吞吐 | > 200万行/秒 |
| 内存缓存命中率 | > 95%（热数据） |
| 压缩比（K线） | > 5:1（Delta-of-Delta + Zstd） |
| 压缩比（Tick） | > 3:1（Gorilla + Zstd） |
| Compaction 写放大 | < 3x |

### 1.7 目录结构

```
src/storage/
├── CMakeLists.txt
├── time_series_store.h          # TimeSeriesStore 主接口
├── time_series_store.cpp
├── time_series_cache.h           # 内存缓存层
├── time_series_cache.cpp
├── column_block.h                # 列式数据块定义
├── column_block.cpp
├── compression/
│   ├── codec.h                  # Codec 抽象接口
│   ├── delta_codec.h/.cpp       # Delta-of-Delta 编码
│   ├── gorilla_codec.h/.cpp     # Gorilla 浮点编码
│   └── zstd_codec.h/.cpp        # Zstd 压缩层
├── segment/
│   ├── segment_writer.h/.cpp    # Segment 文件写入
│   ├── segment_reader.h/.cpp    # Segment 文件读取
│   └── compaction.h/.cpp        # 后台压实
├── wal.h/.cpp                   # Write-Ahead Log
├── manifest.h/.cpp              # 元数据索引管理
└── pybind/
    ├── storage_py.cpp            # pybind11 绑定
    └── CMakeLists.txt
```

---

## 2. 因子计算引擎

### 2.1 概述

因子计算引擎负责因子的注册、调度、增量更新与结果缓存。核心设计围绕 **DAG 依赖图** 组织因子的计算顺序，支持 **SIMD 向量化** 和 **增量计算**，目标是日级别全量因子计算 < 30s，增量更新延迟 < 100μs。

### 2.2 核心接口定义

```cpp
// ===== 因子元信息 =====

using FactorId = uint32_t;

enum class FactorDataType : uint8_t {
    kFloat32 = 0,
    kFloat64 = 1,
    kInt32   = 2,
};

struct FactorDescriptor {
    FactorId        id;
    std::string     name;           // 如 "alpha_001"
    std::string     category;       // 如 "momentum", "volatility"
    FactorDataType  data_type;
    std::vector<FactorId> depends;  // 依赖因子 ID 列表
    bool            incremental;     // 是否支持增量更新
    std::string     description;
};

// ===== 因子注册表 =====

class FactorRegistry {
public:
    // 注册因子描述符
    FactorId register_factor(FactorDescriptor desc);

    // 查询
    const FactorDescriptor& descriptor(FactorId id) const;
    std::vector<FactorId>   all_factors() const;

    // DAG 查询：给定一组目标因子，返回拓扑排序的计算顺序
    std::vector<FactorId> topological_order(
        std::span<const FactorId> targets) const;

    // DAG 查询：返回因子子图（用于并行调度）
    struct ParallelPlan {
        std::vector<std::vector<FactorId>> waves; // 按层并行
    };
    ParallelPlan parallel_order(
        std::span<const FactorId> targets) const;

private:
    ankerl::unordered_dense::map<FactorId, FactorDescriptor> factors_;
};

// ===== 因子计算接口 =====

class IFactorCalculator {
public:
    virtual ~IFactorCalculator() = default;

    virtual FactorId factor_id() const = 0;

    // 全量计算：给定交易日，计算所有股票的因子值
    virtual void compute(const FactorContext& ctx,
                         std::span<float> output) const = 0;

    // 增量更新：仅重新计算新增数据影响的行
    // 默认实现调用全量 compute
    virtual void compute_incremental(const FactorContext& ctx,
                                      std::span<const int> changed_indices,
                                      std::span<float> output) const {
        compute(ctx, output);
    }
};

// ===== 因子上下文（只读数据访问） =====

class FactorContext {
public:
    // 获取依赖因子的全量结果
    std::span<const float> factor_result(FactorId id) const;

    // 获取行情数据（列式）
    std::span<const float> kline_field(DataField field,
                                         std::string_view symbol) const;

    // 获取 universe（当前交易日股票列表）
    std::span<const std::string> universe() const;

    // 当前交易日
    int64_t trading_day() const noexcept;
};

// ===== SIMD 向量化计算工具 =====

namespace simd {

// 一元运算：output[i] = op(input[i])
void abs(std::span<const float> input, std::span<float> output);
void log(std::span<const float> input, std::span<float> output);
void sqrt(std::span<const float> input, std::span<float> output);
void sign(std::span<const float> input, std::span<float> output);

// 二元运算：output[i] = op(lhs[i], rhs[i])
void add(std::span<const float> lhs, std::span<const float> rhs,
         std::span<float> output);
void sub(std::span<const float> lhs, std::span<const float> rhs,
         std::span<float> output);
void mul(std::span<const float> lhs, std::span<const float> rhs,
         std::span<float> output);
void div(std::span<const float> lhs, std::span<const float> rhs,
         std::span<float> output);  // 除零保护：0/0 → NaN

// 滚动窗口：output[i] = op(input[i-window+1 .. i])
void rolling_mean(std::span<const float> input, size_t window,
                   std::span<float> output);
void rolling_std(std::span<const float> input, size_t window,
                  std::span<float> output);
void rolling_max(std::span<const float> input, size_t window,
                  std::span<float> output);
void rolling_min(std::span<const float> input, size_t window,
                  std::span<float> output);
void rolling_rank(std::span<const float> input, size_t window,
                   std::span<float> output);

// Rank：截面排序百分位
void cross_sectional_rank(std::span<const float> input,
                           std::span<float> output);

// 延迟/差分
void delay(std::span<const float> input, size_t period,
            std::span<float> output);
void delta(std::span<const float> input, size_t period,
            std::span<float> output);

} // namespace simd

// ===== 因子缓存 =====

class FactorCache {
public:
    explicit FactorCache(size_t memory_budget_mb);

    // 存储因子计算结果（按 trading_day + factor_id 索引）
    void put(int64_t trading_day,
             FactorId factor_id,
             std::span<const float> data);

    // 查询因子结果；未命中返回空 span
    std::span<const float> get(int64_t trading_day,
                                FactorId factor_id) const;

    // 淘汰旧数据
    void evict_before(int64_t trading_day);

private:
    struct CacheKey {
        int64_t  trading_day;
        FactorId factor_id;
        auto operator<=>(const CacheKey&) const = default;
    };

    // LRU + 分片
    static constexpr size_t kShardCount = 32;
    struct Shard {
        std::shared_mutex rwlock;
        ankerl::unordered_dense::map<CacheKey, std::vector<float>> data;
        size_t memory_used = 0;
    };
    std::array<Shard, kShardCount> shards_;
};

// ===== 因子计算调度器 =====

class FactorScheduler {
public:
    struct Options {
        size_t compute_threads = 0;  // 0 = 自动检测
        size_t cache_memory_mb = 4096;
    };

    FactorScheduler(std::shared_ptr<FactorRegistry> registry,
                    std::shared_ptr<TimeSeriesStore> store,
                    Options opts);

    // 注册因子计算器
    void register_calculator(std::unique_ptr<IFactorCalculator> calc);

    // 全量计算：指定交易日和目标因子集合
    // 返回每个因子的结果
    std::unordered_map<FactorId, std::vector<float>>
    compute_all(int64_t trading_day,
                std::span<const FactorId> targets);

    // 增量更新：行情数据更新后，仅重算受影响的因子
    void compute_incremental(int64_t trading_day,
                              std::span<const FactorId> changed_factors);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
```

### 2.3 数据流描述

```
  全量计算流程（日终批量）：

  ┌──────────────────────────────────────────────────────────┐
  │  因子目标列表: [alpha_101, alpha_045, ...]              │
  └──────────────┬───────────────────────────────────────────┘
                 │
  ┌──────────────v───────────────────────────────────────────┐
  │  FactorRegistry::parallel_order()                         │
  │  输出并行执行计划:                                        │
  │    Wave 0: [close_ret, volume_ma5]    (无依赖)          │
  │    Wave 1: [alpha_001]                (依赖 close_ret)   │
  │    Wave 2: [alpha_101, alpha_045]     (依赖 alpha_001)   │
  └──────────────┬───────────────────────────────────────────┘
                 │
       ┌─────────┴─────────┐
       │    线程池调度       │
       │  Wave 0 并行执行    │──> FactorContext 提供 close/volume
       │  Wave 1 执行       │──> FactorContext 提供 close_ret + alpha_001 结果
       │  Wave 2 并行执行    │──> FactorContext 提供 alpha_001 结果
       └─────────┬─────────┘
                 │
  ┌──────────────v───────────────────────────────────────────┐
  │  结果写入 FactorCache + 返回给调用方                      │
  └──────────────────────────────────────────────────────────┘


  增量更新流程（盘中实时）：

  ┌──────────────┐     ┌───────────────────┐
  │ 新K线数据    │────>│ 确定受影响因子     │────> DAG 子图提取
  └──────────────┘     │ (FactorRegistry   │           │
                       │  反向依赖索引)      │           v
                       └───────────────────┘    仅重算受影响的链路
                                                       │
                       ┌───────────────────┐           │
                       │ IFactorCalculator  │<──────────┘
                       │ ::compute_incremental│
                       └───────────────────┘
```

### 2.4 DAG 结构示例

```
因子依赖图（示例）：

  [close]  [volume]               行情原始数据（叶节点）
      │        │
  [close_ret] [vol_ma5]           基础因子
      │        │
  [alpha_001]──┘                  一级因子
      │
  [alpha_101]                    二级因子

并行执行计划：
  Wave 0: close_ret, vol_ma5    ← 可并行（无依赖）
  Wave 1: alpha_001             ← 依赖 Wave 0
  Wave 2: alpha_101             ← 依赖 Wave 1

每个 Wave 内的因子由线程池并行计算，
Wave 之间有依赖屏障同步。
```

### 2.5 线程模型与并发策略

| 组件 | 线程模型 | 并发机制 |
|------|---------|---------|
| 调度器 | 主线程编排 | 解析 DAG → 生成并行计划，提交给线程池 |
| 全量计算 | 线程池（CPU 核数） | 按 Wave 并行：同一 Wave 内因子并行计算，Wave 间 barrier 同步 |
| 增量更新 | 事件驱动（消费行情推送） | 因子 DAG 子图重算，线程池并行 |
| 因子缓存 | 分片 32 个 `shared_mutex` | 读多写少场景，读共享锁 |
| SIMD | 单线程内部 | 每个 `IFactorCalculator::compute` 内部使用 AVX2/AVX-512 |

**关键设计**：
- 因子之间无写冲突（每个因子写自己独立的输出缓冲区），天然适合并行
- `FactorContext` 通过 `span<const float>` 提供只读视图，避免数据拷贝
- SIMD 函数编译期通过 `#pragma GCC target("avx2")` 确保生成向量化代码，运行时通过 `cpuid` 检测降级

### 2.6 性能目标

| 指标 | 目标值 |
|------|--------|
| 全量因子计算（5000股 × 200因子） | < 30s |
| 增量因子更新延迟 | < 100μs |
| 因子缓存命中率 | > 90% |
| SIMD 加速比（vs 标量） | > 4x（AVX2） |
| DAG 调度开销 | < 1ms |
| 内存占用（5000股 × 200因子 × 250天） | < 8GB（float32） |

### 2.7 目录结构

```
src/factor/
├── CMakeLists.txt
├── registry/
│   ├── factor_registry.h/.cpp       # 因子注册与 DAG 管理
│   └── factor_descriptor.h          # FactorDescriptor 定义
├── compute/
│   ├── factor_calculator.h          # IFactorCalculator 接口
│   ├── factor_context.h/.cpp        # FactorContext 数据访问
│   ├── factor_scheduler.h/.cpp      # 调度器：DAG 编排 + 线程池
│   └── simd_ops.h/.cpp             # SIMD 向量化运算库
├── cache/
│   ├── factor_cache.h/.cpp          # 因子结果缓存
│   └── lru_policy.h                # LRU 淘汰策略
├── factors/                         # 具体因子实现（示例）
│   ├── alpha_001.h/.cpp
│   ├── alpha_045.h/.cpp
│   ├── momentum/
│   │   ├── close_ret.h/.cpp
│   │   └── vol_ma.h/.cpp
│   └── volatility/
│       ├── realized_vol.h/.cpp
│       └── bollinger.h/.cpp
└── pybind/
    ├── factor_py.cpp                # pybind11 绑定
    └── CMakeLists.txt
```

---

## 3. 执行引擎

### 3.1 概述

执行引擎负责订单全生命周期管理、算法交易执行和券商接口对接。核心设计基于 **订单状态机** 和 **策略模式**，支持TWAP/VWAP/Pov等算法交易，并通过抽象接口层与不同券商系统解耦。

### 3.2 核心接口定义

```cpp
// ===== 订单数据结构 =====

enum class OrderSide : uint8_t {
    kBuy  = 0,
    kSell = 1,
};

enum class OrderType : uint8_t {
    kLimit       = 0,
    kMarket      = 1,
    kLimitCancel = 2,   // 深交所废单改撤单
};

enum class OrderStatus : uint8_t {
    kPendingSubmit = 0,   // 待提交
    kSubmitted     = 1,   // 已提交（券商收到）
    kPartialFilled = 2,   // 部分成交
    kFilled        = 3,   // 全部成交
    kCancelled     = 4,   // 已撤单
    kRejected      = 5,   // 券商拒绝
    kExpired       = 6,   // 过期
};

enum class_algo : uint8_t {
    kManual = 0,
    kTWAP   = 1,
    kVWAP   = 2,
    kPov    = 3,
};

struct OrderId {
    uint64_t internal_id;   // 系统内部 ID
    uint64_t broker_id;     // 券商委托号
};

struct Order {
    OrderId       id;
    std::string   symbol;          // 如 "000001.SZ"
    OrderSide     side;
    OrderType     type;
    OrderStatus   status;
    int32_t       price;           // ×10000 定点
    int64_t       quantity;
    int64_t       filled_quantity;
    int64_t       remaining_quantity;
    AlgoType      algo_type;
    int64_t       create_time_us;  // 微秒时间戳
    int64_t       update_time_us;
    std::string   reject_reason;
};

// ===== 订单状态机 =====

class OrderStateMachine {
public:
    // 状态转换表：
    //   PendingSubmit -> Submitted    (on_broker_ack)
    //   PendingSubmit -> Rejected      (on_broker_reject)
    //   Submitted     -> PartialFilled (on_partial_fill)
    //   Submitted     -> Filled        (on_fill)
    //   Submitted     -> Cancelled      (on_cancel_ack)
    //   PartialFilled -> PartialFilled (on_partial_fill)
    //   PartialFilled -> Filled        (on_fill)
    //   PartialFilled -> Cancelled      (on_cancel_ack)

    // 推进状态，返回是否合法
    bool transition(Order& order, OrderStatus new_status,
                    const std::string& reason = "");

    // 合法性检查
    static bool is_valid_transition(OrderStatus from, OrderStatus to);

private:
    // 状态转移矩阵
    static constexpr bool kTransitionMatrix[7][7] = {
        // PSub  Sub   PF    Fill  Cxl   Rej   Exp
        {  false, true, false, false, false, true, false}, // PendingSubmit
        {  false, false, true, true, true, false, false},  // Submitted
        {  false, false, true, true, true, false, false},  // PartialFilled
        {  false, false, false, false, false, false, false}, // Filled (终态)
        {  false, false, false, false, false, false, false}, // Cancelled (终态)
        {  false, false, false, false, false, false, false}, // Rejected (终态)
        {  false, false, false, false, false, false, false}, // Expired (终态)
    };
};

// ===== 订单管理器 =====

class OrderManager {
public:
    explicit OrderManager(size_t shard_count = 16);

    // 创建订单
    OrderId create_order(const Order& template_order);

    // 修改订单（改价/改量）
    bool amend_order(OrderId id, int32_t new_price, int64_t new_quantity);

    // 撤销订单
    bool cancel_order(OrderId id);

    // 查询
    const Order* get_order(OrderId id) const;
    std::vector<const Order*> query_orders(std::string_view symbol) const;
    std::vector<const Order*> query_orders(OrderStatus status) const;

    // 回报处理（由券商接口回调）
    void on_order_ack(OrderId id, uint64_t broker_id);
    void on_order_reject(OrderId id, std::string reason);
    void on_order_fill(OrderId id, int64_t fill_quantity, int32_t fill_price);
    void on_order_cancel(OrderId id);

    // 设置状态变更回调
    using OrderCallback = std::function<void(const Order&)>;
    void set_callback(OrderCallback cb);

private:
    struct Shard {
        std::shared_mutex rwlock;
        ankerl::unordered_dense::map<uint64_t, Order> orders;
    };
    std::array<Shard, 16> shards_;
    OrderStateMachine state_machine_;
    OrderCallback callback_;
    std::atomic<uint64_t> next_id_{1};
};

// ===== 算法交易接口 =====

struct AlgoParams {
    // TWAP 参数
    int64_t twap_duration_sec = 300;     // 拆单持续时间
    int64_t twap_interval_sec = 30;      // 下单间隔
    // VWAP 参数
    std::vector<double> vwap_volume_profile; // 分钟级成交量占比
    // Pov 参数
    double pov_participation_rate = 0.1; // 参与率
    // 通用参数
    double   slippage_bps     = 5.0;    // 滑点容忍（基点）
    int64_t  max_order_size    = 10000;  // 单笔最大委托量
    int64_t  urgency           = 5;      // 紧急度 1-10
};

class IAlgoStrategy {
public:
    virtual ~IAlgoStrategy() = default;

    virtual AlgoType algo_type() const = 0;

    // 初始化算法（母单）
    virtual void init(const Order& parent_order,
                      const AlgoParams& params) = 0;

    // 定时回调（每次收到行情或有成交回报时触发）
    // 返回本轮需要发出的子单列表；空列表表示本轮不下单
    virtual std::vector<Order> on_tick(int64_t timestamp_us) = 0;

    // 成交回报回调
    virtual void on_fill(const Order& child_order) = 0;

    // 算法是否完成
    virtual bool is_finished() const = 0;

    // 获取算法执行进度 [0.0, 1.0]
    virtual double progress() const = 0;
};

// TWAP 实现
class TwapAlgo : public IAlgoStrategy { /* ... */ };

// VWAP 实现
class VwapAlgo : public IAlgoStrategy { /* ... */ };

// Pov 实现（Percentage of Volume）
class PovAlgo : public IAlgoStrategy { /* ... */ };

// ===== 算法交易引擎 =====

class AlgoEngine {
public:
    using StrategyFactory = std::function<std::unique_ptr<IAlgoStrategy>()>;

    AlgoEngine(std::shared_ptr<OrderManager> order_mgr,
               std::shared_ptr<IBrokerGateway> broker);

    // 注册算法工厂
    void register_algo(AlgoType type, StrategyFactory factory);

    // 启动算法交易（母单）
    // 返回母单 ID，后续可通过此 ID 追踪
    OrderId start_algo(const Order& parent_order, const AlgoParams& params);

    // 停止算法交易
    void stop_algo(OrderId parent_id);

    // 查询算法执行进度
    double algo_progress(OrderId parent_id) const;

    // 主循环（由消息总线驱动）
    void on_market_data(const MarketDataEvent& event);
    void on_order_report(const OrderReportEvent& event);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ===== 券商接口抽象层 =====

class IBrokerGateway {
public:
    virtual ~IBrokerGateway() = default;

    // 连接管理
    virtual void connect(const std::string& endpoint) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;

    // 订单操作
    virtual void submit_order(const Order& order) = 0;
    virtual void amend_order(OrderId id, int32_t new_price,
                              int64_t new_quantity) = 0;
    virtual void cancel_order(OrderId id) = 0;

    // 查询
    virtual std::vector<Order> query_open_orders(std::string_view symbol) = 0;
    virtual double query_position(std::string_view symbol) = 0;

    // 事件订阅
    using OrderReportCallback = std::function<void(const OrderReportEvent&)>;
    using MarketDataCallback  = std::function<void(const MarketDataEvent&)>;

    virtual void set_order_report_callback(OrderReportCallback cb) = 0;
    virtual void set_market_data_callback(MarketDataCallback cb) = 0;
};

// XTP 券商实现
class XtpBrokerGateway : public IBrokerGateway { /* ... */ };
```

### 3.3 数据流描述

```
  算法交易执行流（以 TWAP 为例）：

  ┌──────────────────────────────────────────────────┐
  │  策略层/手动下单 → 提交母单                       │
  │  Order: BUY 000001.SZ 100000股 @ 10.50          │
  └──────────────┬───────────────────────────────────┘
                 │
  ┌──────────────v───────────────────────────────────┐
  │  AlgoEngine::start_algo()                        │
  │  创建 TwapAlgo 策略实例                           │
  │  母单写入 OrderManager                           │
  └──────────────┬───────────────────────────────────┘
                 │
       ┌─────────┴──────────┐
       │  定时 / 行情驱动    │
       │  TwapAlgo::on_tick │────> 计算本轮子单量
       │                    │      (100000 / 10 = 10000/轮)
       └─────────┬──────────┘
                 │
  ┌──────────────v───────────────────────────────────┐
  │  子单创建 → OrderManager::create_order()          │
  │  子单提交 → IBrokerGateway::submit_order()        │
  └──────────────┬───────────────────────────────────┘
                 │
  ┌──────────────v───────────────────────────────────┐
  │  券商回报 → on_order_fill()                        │
  │  状态更新 → OrderStateMachine::transition()        │
  │  通知算法 → TwapAlgo::on_fill()                    │
  │  更新母单进度                                      │
  └──────────────────────────────────────────────────┘
```

### 3.4 订单状态机

```
                  ┌────────────────┐
                  │ PendingSubmit  │
                  └───────┬────────┘
                     ┌────┴────┐
                ack  │         │ reject
                     v         v
              ┌──────────┐  ┌─────────┐
              │ Submitted │  │ Rejected │ (终态)
              └─────┬─────┘  └─────────┘
           ┌────┬───┴───┬────┐
    partial│    │fill  │  cancel
          fill│    v     │     │
           ┌──┴─────────┐   v
           │PartialFilled│  ┌──────────┐
           └─────┬───────┘  │ Cancelled │ (终态)
          ┌──────┴──────┐   └──────────┘
     fill│    cancel    │
          v             v
   ┌─────────┐   ┌──────────┐
   │  Filled  │   │ Cancelled │ (终态)
   └─────────┘   └──────────┘
```

### 3.5 线程模型与并发策略

| 组件 | 线程模型 | 并发机制 |
|------|---------|---------|
| 算法引擎主循环 | 1 个事件循环线程 | 由消息总线驱动，串行处理行情事件 |
| 子单生成 | 事件循环线程 | 回调 `IAlgoStrategy::on_tick()`，同步生成子单 |
| 订单管理 | 分片 16 个 `shared_mutex` | 查询用读锁，创建/修改用写锁 |
| 券商接口 | 1 个发送线程 + 1 个接收线程 | 发送队列 (MPSC)，接收线程回调到事件循环 |

**关键设计**：
- 算法引擎核心逻辑在 **单线程事件循环** 中执行，避免锁竞争
- OrderManager 使用 **分片锁** 支持并发查询
- 券商接口的回调通过 **消息总线** 投递到事件循环线程，保证时序一致
- 子单的创建和状态变更是 **原子操作**（写锁粒度限于单个分片）

### 3.6 性能目标

| 指标 | 目标值 |
|------|--------|
| 订单创建延迟 | < 1μs |
| 订单状态更新延迟 | < 5μs |
| 下单到券商延迟 | < 100μs |
| 算法交易 TWAP 偏差 | < 2bps（vs 理想 TWAP） |
| 算法交易 VWAP 偏差 | < 3bps（vs 市场 VWAP） |
| 订单管理器并发查询 | > 100万 QPS |
| 系统订单容量 | > 10万笔/分钟 |

### 3.7 目录结构

```
src/execution/
├── CMakeLists.txt
├── order/
│   ├── order.h                    # Order 数据结构定义
│   ├── order_manager.h/.cpp       # 订单管理器
│   ├── order_state_machine.h/.cpp # 订单状态机
│   └── order_id.h                 # OrderId 定义
├── algo/
│   ├── algo_engine.h/.cpp         # 算法交易引擎
│   ├── algo_strategy.h            # IAlgoStrategy 接口
│   ├── twap_algo.h/.cpp           # TWAP 算法
│   ├── vwap_algo.h/.cpp           # VWAP 算法
│   └── pov_algo.h/.cpp            # Pov 算法
├── broker/
│   ├── broker_gateway.h           # IBrokerGateway 抽象接口
│   ├── xtp_gateway.h/.cpp         # XTP 券商实现
│   └── mock_gateway.h/.cpp        # 模拟券商（测试用）
└── pybind/
    ├── execution_py.cpp            # pybind11 绑定
    └── CMakeLists.txt
```

---

## 4. 风控引擎

### 4.1 概述

风控引擎负责实盘交易中的实时风险监控和控制。核心设计采用 **规则引擎** 架构，风控规则可配置、可热加载；支持 **实时检查**（每笔交易前拦截）和 **定时巡检**（定期扫描全持仓）；提供 **风控熔断** 机制，在极端情况下自动切断交易。

### 4.2 核心接口定义

```cpp
// ===== 风控规则定义 =====

using RuleId = uint32_t;

enum class RiskLevel : uint8_t {
    kGreen  = 0,  // 安全
    kYellow = 1,  // 警告
    kRed    = 2,  // 危险
};

enum class RuleSeverity : uint8_t {
    kInfo    = 0,  // 仅记录日志
    kWarning = 1,  // 警告但不阻止
    kBlock   = 2,  // 阻止交易
    kCircuit = 3,  // 触发熔断
};

enum class RuleTrigger : uint8_t {
    kPreOrder  = 0,  // 下单前检查
    kPostTrade = 1,  // 成交后检查
    kPeriodic  = 2,  // 定时巡检
};

struct RuleViolation {
    RuleId      rule_id;
    RiskLevel   risk_level;
    RuleSeverity severity;
    std::string message;       // 如 "个股 000001.SZ 集中度达 12.5%，超过阈值 10%"
    double      current_value; // 当前实际值
    double      threshold;     // 规则阈值
};

struct RiskCheckResult {
    bool  passed;                   // 是否通过
    std::vector<RuleViolation> violations;
};

// ===== 风控上下文（检查时的只读数据） =====

struct PositionInfo {
    std::string symbol;
    double      quantity;
    double      market_value;
    double      cost_price;
    double      current_price;
    double      unrealized_pnl;
    double      weight;       // 占总资产比例
};

struct PortfolioSnapshot {
    double total_value;       // 总资产
    double cash;              // 可用现金
    double max_drawdown;      // 最大回撤
    double daily_pnl_pct;    // 日内盈亏比例
    std::span<const PositionInfo> positions;
};

struct OrderContext {
    Order     order;
    PortfolioSnapshot portfolio; // 下单前快照
};

// ===== 风控规则接口 =====

class IRiskRule {
public:
    virtual ~IRiskRule() = default;

    virtual RuleId        id() const = 0;
    virtual std::string   name() const = 0;
    virtual RuleTrigger   trigger() const = 0;
    virtual RuleSeverity  severity() const = 0;

    // 事前检查（pre-order）
    virtual RiskCheckResult check(const OrderContext& ctx) const { return {true, {}}; }

    // 事后检查（post-trade）
    virtual RiskCheckResult check_post(const PortfolioSnapshot& portfolio) const { return {true, {}}; }

    // 定时巡检
    virtual RiskCheckResult check_periodic(const PortfolioSnapshot& portfolio) const { return {true, {}}; }

    // 规则参数热更新
    virtual void update_params(const std::unordered_map<std::string, double>& params) = 0;
};

// ===== 具体规则示例 =====

// 回撤风控
class MaxDrawdownRule : public IRiskRule {
public:
    MaxDrawdownRule(double threshold_pct = 15.0, RuleSeverity severity = RuleSeverity::kBlock);

    RuleId       id() const override { return 1001; }
    std::string  name() const override { return "max_drawdown"; }
    RuleTrigger  trigger() const override { return RuleTrigger::kPeriodic; }
    RuleSeverity severity() const override { return severity_; }

    RiskCheckResult check_periodic(const PortfolioSnapshot& portfolio) const override;
    void update_params(const std::unordered_map<std::string, double>& params) override;

private:
    double threshold_pct_;
    RuleSeverity severity_;
};

// 行业暴露风控
class IndustryExposureRule : public IRiskRule {
public:
    IndustryExposureRule(double max_exposure_pct = 30.0,
                          RuleSeverity severity = RuleSeverity::kBlock);

    RuleId       id() const override { return 1002; }
    std::string  name() const override { return "industry_exposure"; }
    RuleTrigger  trigger() const override { return RuleTrigger::kPeriodic; }

    RiskCheckResult check_periodic(const PortfolioSnapshot& portfolio) const override;
    void update_params(const std::unordered_map<std::string, double>& params) override;

private:
    double max_exposure_pct_;
    RuleSeverity severity_;
};

// 个股集中度风控
class SingleStockConcentrationRule : public IRiskRule {
public:
    SingleStockConcentrationRule(double max_weight_pct = 10.0,
                                   RuleSeverity severity = RuleSeverity::kBlock);

    RuleId       id() const override { return 1003; }
    std::string  name() const override { return "single_stock_concentration"; }
    RuleTrigger  trigger() const override { return RuleTrigger::kPreOrder; }

    RiskCheckResult check(const OrderContext& ctx) const override;
    void update_params(const std::unordered_map<std::string, double>& params) override;

private:
    double max_weight_pct_;
    RuleSeverity severity_;
};

// 日亏损风控
class DailyLossRule : public IRiskRule {
public:
    DailyLossRule(double max_loss_pct = 5.0, RuleSeverity severity = RuleSeverity::kBlock);

    RuleId       id() const override { return 1004; }
    std::string  name() const override { return "daily_loss"; }
    RuleTrigger  trigger() const override { return RuleTrigger::kPostTrade | RuleTrigger::kPeriodic; }

    RiskCheckResult check_post(const PortfolioSnapshot& portfolio) const override;
    RiskCheckResult check_periodic(const PortfolioSnapshot& portfolio) const override;
    void update_params(const std::unordered_map<std::string, double>& params) override;

private:
    double max_loss_pct_;
    RuleSeverity severity_;
};

// ===== 风控熔断器 =====

class CircuitBreaker {
public:
    struct Config {
        double cooldown_seconds = 60.0;  // 熔断冷却时间
        int    max_triggers_per_day = 3;  // 每日最大触发次数
    };

    explicit CircuitBreaker(Config config);

    // 触发熔断：停止所有交易
    // reason: 熔断原因
    void trigger(std::string_view reason);

    // 尝试恢复：冷却期结束后可恢复
    // 返回 true 表示恢复成功
    bool try_recover();

    // 查询状态
    bool is_tripped() const noexcept;
    const std::string& trip_reason() const noexcept;
    int  daily_trigger_count() const noexcept;

    // 设置回调（熔断触发/恢复时通知）
    using Callback = std::function<void(bool tripped, const std::string& reason)>;
    void set_callback(Callback cb);

private:
    Config                config_;
    std::atomic<bool>     tripped_{false};
    std::string           trip_reason_;
    int64_t               trip_timestamp_us_ = 0;
    std::atomic<int>      daily_trigger_count_{0};
    Callback              callback_;
    mutable std::mutex    mutex_;
};

// ===== 风控引擎主体 =====

class RiskEngine {
public:
    struct Options {
        uint32_t periodic_check_interval_ms = 1000; // 定时巡检间隔
        bool     enable_circuit_breaker     = true;
    };

    RiskEngine(std::shared_ptr<CircuitBreaker> breaker, Options opts);

    // ===== 规则管理 =====

    // 注册规则
    void register_rule(std::unique_ptr<IRiskRule> rule);

    // 移除规则
    void remove_rule(RuleId id);

    // 动态更新规则参数（热加载）
    void update_rule_params(RuleId id,
                             const std::unordered_map<std::string, double>& params);

    // ===== 风控检查 =====

    // 事前检查（下单前调用，同步阻塞）
    // 返回：通过/阻止 + 违规列表
    RiskCheckResult pre_order_check(const OrderContext& ctx);

    // 事后检查（成交后回调）
    void post_trade_check(const PortfolioSnapshot& portfolio);

    // 定时巡检（由定时器周期调用）
    void periodic_check(const PortfolioSnapshot& portfolio);

    // ===== 熔断控制 =====

    void circuit_break(std::string_view reason);
    bool is_circuit_broken() const noexcept;

    // ===== 事件回调 =====

    using RiskCallback = std::function<void(const RiskCheckResult&)>;
    void set_risk_callback(RiskCallback cb);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
```

### 4.3 数据流描述

```
  下单前风控检查流程：

  ┌────────────────────────────────────────────────┐
  │  AlgoEngine / 手动下单 → 提交订单               │
  └──────────────┬─────────────────────────────────┘
                 │
  ┌──────────────v─────────────────────────────────┐
  │  RiskEngine::pre_order_check()                  │
  │  1. 构造 OrderContext（含组合快照）              │
  │  2. 遍历所有 PreOrder 规则                       │
  │  3. 短路原则：任一 Block 规则触发即阻止          │
  └──────────────┬─────────────────────────────────┘
                 │
        ┌────────┴────────┐
        │ passed=true     │ passed=false
        v                 v
  ┌────────────┐   ┌──────────────┐
  │ 继续下单    │   │ 记录违规日志  │──> 通知策略层
  └────────────┘   │ 阻止订单      │──> 触发风控告警
                   └──────────────┘


  定时巡检流程：

  ┌──────────────┐     每 1s
  │  定时器触发    │────> RiskEngine::periodic_check()
  └──────────────┘           │
                               v
                    ┌────────────────────────┐
                    │ 遍历 Periodic 规则      │
                    │ 计算组合风险指标         │
                    └────────────┬───────────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
                    v            v            v
               Green/Yellow  Red/Block    CircuitBreak
               记录日志      阻止新单     熔断（停止所有交易）


  熔断恢复流程：

  ┌──────────────┐     等待冷却期
  │  熔断触发    │────> 检查冷却时间到 → try_recover()
  └──────────────┘           │
                    ┌────────┴────────┐
                    │ 恢复成功         │ 每日触发过多 → 次日恢复
                    │ 允许交易         │
                    └─────────────────┘
```

### 4.4 线程模型与并发策略

| 组件 | 线程模型 | 并发机制 |
|------|---------|---------|
| pre_order_check | 调用方线程（同步阻塞） | 无锁设计，规则列表 `shared_mutex` 保护；读多写极少 |
| periodic_check | 独立定时线程 | 每 1s 一次，持有读锁遍历规则 |
| 规则热更新 | 配置线程 | 写锁保护，更新参数原子化 |
| 熔断器 | `atomic<bool>` | 无锁快速检查（热路径零开销） |

**关键设计**：
- **热路径零开销**：下单前的风控检查在调用方线程同步执行，熔断状态通过 `atomic<bool>` 检查，正常情况下仅为一次原子读（< 1ns）
- **读多写少**：规则参数热更新频率极低，使用 `shared_mutex` 保护规则列表
- **熔断原子操作**：熔断触发/恢复通过 `atomic<bool>` + `mutex` 组合，保证有序性

### 4.5 性能目标

| 指标 | 目标值 |
|------|--------|
| pre_order_check 延迟 | < 5μs（正常路径） |
| pre_order_check 延迟 | < 50μs（含组合快照构建） |
| periodic_check 延迟 | < 10ms（100 条规则） |
| 规则热更新延迟 | < 1ms |
| 熔断检查延迟 | < 1ns（原子读） |
| 熔断恢复延迟 | < 1ms |
| 风控事件通知延迟 | < 100μs |

### 4.6 目录结构

```
src/risk/
├── CMakeLists.txt
├── risk_engine.h/.cpp             # 风控引擎主体
├── risk_rule.h                    # IRiskRule 接口定义
├── circuit_breaker.h/.cpp         # 风控熔断器
├── rules/
│   ├── max_drawdown_rule.h/.cpp   # 回撤风控规则
│   ├── industry_exposure_rule.h/.cpp  # 行业暴露规则
│   ├── single_stock_concentration.h/.cpp  # 个股集中度规则
│   └── daily_loss_rule.h/.cpp     # 日亏损规则
├── risk_context.h                 # 风控上下文数据结构
├── risk_config.h/.cpp             # 风控配置管理（热加载）
└── pybind/
    ├── risk_py.cpp                 # pybind11 绑定
    └── CMakeLists.txt
```

---

## 5. 消息总线与事件系统

### 5.1 概述

消息总线是C++核心引擎间通信的中枢，采用 **发布/订阅** 模式，支持行情推送、交易信号、风控告警等多种事件类型。设计目标是 **低延迟**（< 1μs 同步派发）和 **高吞吐**（> 1000万事件/秒），同时保证事件的有序性和可靠性。

### 5.2 核心接口定义

```cpp
// ===== 事件类型定义 =====

using EventTypeId = uint32_t;

// 事件类型注册宏（编译期类型安全）
#define DEFINE_EVENT_TYPE(EventClass, id)                    \
    static constexpr EventTypeId kEventTypeId = id;          \
    EventTypeId event_type_id() const override { return id; }

// 事件基类
class Event {
public:
    virtual ~Event() = default;
    virtual EventTypeId event_type_id() const = 0;
    virtual std::string  event_name() const = 0;

    int64_t timestamp_us() const noexcept { return timestamp_us_; }
    uint64_t sequence() const noexcept { return sequence_; }

protected:
    int64_t  timestamp_us_;   // 事件产生时间（微秒）
    uint64_t sequence_;       // 全局单调递增序号
};

// ===== 具体事件类型 =====

class MarketDataEvent : public Event {
public:
    DEFINE_EVENT_TYPE(MarketDataEvent, 1);

    std::string symbol;
    int32_t     last_price;    // ×10000
    int64_t     volume;
    int32_t     bid_price1;
    int32_t     ask_price1;
    int32_t     bid_vol1;
    int32_t     ask_vol1;
};

class KlineEvent : public Event {
public:
    DEFINE_EVENT_TYPE(KlineEvent, 2);

    std::string symbol;
    DataType    kline_type;
    KlineRow    kline;
};

class TradeSignalEvent : public Event {
public:
    DEFINE_EVENT_TYPE(TradeSignalEvent, 3);

    std::string strategy_id;
    std::string symbol;
    OrderSide   side;
    double      target_weight;   // 目标仓位权重
    double      confidence;      // 信号置信度
};

class OrderReportEvent : public Event {
public:
    DEFINE_EVENT_TYPE(OrderReportEvent, 4);

    OrderId     order_id;
    OrderStatus status;
    int64_t     filled_quantity;
    int32_t     filled_price;
    std::string reject_reason;
};

class RiskAlertEvent : public Event {
public:
    DEFINE_EVENT_TYPE(RiskAlertEvent, 5);

    RuleId      rule_id;
    RiskLevel   risk_level;
    RuleSeverity severity;
    std::string message;
};

class FactorUpdateEvent : public Event {
public:
    DEFINE_EVENT_TYPE(FactorUpdateEvent, 6);

    FactorId    factor_id;
    int64_t     trading_day;
    // 因子结果通过 FactorCache 获取，事件仅通知更新
};

// ===== 事件总线过滤器 =====

class IEventFilter {
public:
    virtual ~IEventFilter() = default;
    virtual bool accept(const Event& event) const = 0;
};

// 符号过滤器：只接收特定标的的事件
class SymbolFilter : public IEventFilter {
public:
    explicit SymbolFilter(std::span<const std::string> symbols);
    bool accept(const Event& event) const override;
private:
    ankerl::unordered_dense::set<std::string> symbols_;
};

// 事件类型过滤器
class EventTypeFilter : public IEventFilter {
public:
    explicit EventTypeFilter(std::span<const EventTypeId> types);
    bool accept(const Event& event) const override;
private:
    std::vector<EventTypeId> types_;
};

// ===== 订阅者接口 =====

using SubscriptionId = uint64_t;

class IEventSubscriber {
public:
    virtual ~IEventSubscriber() = default;

    // 处理事件（同步调用，在发布线程上执行）
    // 注意：必须快速返回，不可做阻塞操作
    virtual void on_event(const Event& event) = 0;
};

// ===== 事件总线核心 =====

class EventBus {
public:
    struct Options {
        size_t subscriber_shard_count = 64;  // 订阅者分片数
        size_t history_buffer_size    = 1024; // 事件回放缓冲区大小
        bool   enable_profiling       = false;
    };

    explicit EventBus(Options opts = {});

    ~EventBus();

    // ===== 发布接口 =====

    // 同步发布：在调用线程上直接派发给所有订阅者
    // 延迟 < 1μs（热路径零分配）
    void publish(std::unique_ptr<Event> event);

    // 异步发布：投递到内部队列，由后台线程派发
    // 适用于高频场景，避免发布者阻塞
    void publish_async(std::unique_ptr<Event> event);

    // ===== 订阅接口 =====

    // 订阅事件类型（无过滤）
    SubscriptionId subscribe(EventTypeId type,
                              IEventSubscriber* subscriber);

    // 订阅事件类型（带过滤）
    SubscriptionId subscribe(EventTypeId type,
                              IEventSubscriber* subscriber,
                              std::unique_ptr<IEventFilter> filter);

    // 取消订阅
    void unsubscribe(SubscriptionId id);

    // ===== 管理接口 =====

    // 回放：向新订阅者重放最近的历史事件
    void replay_history(SubscriptionId id, size_t count);

    // 统计信息
    struct Stats {
        uint64_t total_published;
        uint64_t total_delivered;
        uint64_t avg_publish_latency_ns;
        uint64_t queue_depth;
    };
    Stats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// ===== 类型安全的事件总线包装 =====

template<typename EventType>
class TypedEventBus {
public:
    explicit TypedEventBus(EventBus& bus) : bus_(bus) {}

    void publish(std::unique_ptr<EventType> event) {
        bus_.publish(std::move(event));
    }

    void publish_async(std::unique_ptr<EventType> event) {
        bus_.publish_async(std::move(event));
    }

    using TypedCallback = std::function<void(const EventType&)>;
    SubscriptionId subscribe(TypedCallback callback,
                              std::unique_ptr<IEventFilter> filter = nullptr) {
        auto* sub = new TypedCallbackSubscriber<EventType>(std::move(callback));
        // 注意：生命周期管理需外部保证
        return bus_.subscribe(EventType::kEventTypeId, sub, std::move(filter));
    }

    void unsubscribe(SubscriptionId id) {
        bus_.unsubscribe(id);
    }

private:
    EventBus& bus_;
};

// ===== 便捷类型别名 =====

using MarketDataBus  = TypedEventBus<MarketDataEvent>;
using KlineBus       = TypedEventBus<KlineEvent>;
using TradeSignalBus = TypedEventBus<TradeSignalEvent>;
using OrderReportBus = TypedEventBus<OrderReportEvent>;
using RiskAlertBus   = TypedEventBus<RiskAlertEvent>;
using FactorUpdateBus = TypedEventBus<FactorUpdateEvent>;
```

### 5.3 数据流描述

```
  同步发布路径（低延迟）：

  ┌─────────────┐     publish()     ┌────────────────────────────────┐
  │ 行情接收线程 │─────────────────>│  EventBus::publish()           │
  └─────────────┘                   │                                │
                                    │  1. 获取事件类型的订阅者列表     │
  ┌─────────────┐                   │  2. 遍历过滤器                   │
  │ 策略线程     │─────────┐        │  3. 直接调用 on_event()         │
  └─────────────┘         │        │     （在发布线程上执行）          │
                           │        └────────────┬───────────────────┘
  ┌─────────────┐          │                     │
  │ 风控检查线程 │──────────┤         ┌───────────┼────────────┐
  └─────────────┘          │         │           │            │
                            │         v           v            v
                     ┌──────┴──┐ ┌────────┐ ┌────────┐ ┌──────────┐
                     │ 数据存储 │ │因子引擎 │ │风控引擎│ │WebSocket │
                     │ (写入)  │ │(增量更新)│ │(实时检查)│ │(推送前端) │
                     └─────────┘ └────────┘ └────────┘ └──────────┘


  异步发布路径（高吞吐）：

  ┌─────────────┐                            ┌─────────────────┐
  │ 高频行情源   │── publish_async() ──────>  │  MPSC 无锁队列   │
  │ (每秒百万级) │                            │  (批量攒包)      │
  └─────────────┘                            └────────┬────────┘
                                                        │ 派发线程
                                              ┌─────────v──────────┐
                                              │  批量取出事件       │
                                              │  遍历订阅者派发     │
                                              └────────────────────┘
```

### 5.4 线程模型与并发策略

| 组件 | 线程模型 | 并发机制 |
|------|---------|---------|
| 同步发布 | 调用方线程直接派发 | 读锁遍历订阅者列表；无锁 `MPSC` 发布函数 |
| 异步发布 | 1 个派发线程 | MPSC 无锁队列，批量取出后依次派发 |
| 订阅者管理 | 分片 64 个 `shared_mutex` | 读多写少：订阅/取消订阅用写锁，遍历派发用读锁 |
| 事件回放 | 调用方线程 | Ring Buffer 无锁读取历史事件 |

**关键设计**：
- **同步发布热路径零分配**：`publish()` 在调用方线程直接遍历订阅者并调用回调，不创建任何临时对象
- **分片订阅者锁**：64 个分片减少锁争用，不同事件类型的订阅者分布在不同分片中
- **异步模式批量处理**：派发线程批量取出事件（减少队列空转），单次可处理 256-1024 个事件
- **事件回放**：新订阅者可选回放最近 N 个事件，通过 Ring Buffer 实现（无锁读取）

### 5.5 性能目标

| 指标 | 目标值 |
|------|--------|
| 同步派发延迟 | < 1μs（单订阅者） |
| 同步派发延迟 | < 5μs（10 订阅者） |
| 异步派发延迟 | < 10μs（端到端） |
| 吞吐量（同步） | > 1000万事件/秒 |
| 吞吐量（异步） | > 500万事件/秒 |
| 事件回放延迟 | < 100μs |
| 订阅/取消订阅 | < 10μs |

### 5.6 目录结构

```
src/event/
├── CMakeLists.txt
├── event.h                    # Event 基类和事件类型定义
├── event_bus.h/.cpp           # EventBus 核心实现
├── event_filter.h             # IEventFilter 和过滤器实现
├── typed_event_bus.h          # TypedEventBus 模板
├── events/
│   ├── market_data_event.h    # 行情事件
│   ├── kline_event.h          # K线事件
│   ├── trade_signal_event.h   # 交易信号事件
│   ├── order_report_event.h  # 订单回报事件
│   ├── risk_alert_event.h    # 风控告警事件
│   └── factor_update_event.h  # 因子更新事件
├── queue/
│   ├── mpsc_queue.h           # MPSC 无锁队列
│   └── ring_buffer.h          # 事件回放环形缓冲区
└── pybind/
    ├── event_py.cpp            # pybind11 绑定
    └── CMakeLists.txt
```

---

## 6. 跨模块集成

### 6.1 模块依赖关系

```
                    ┌──────────────────────────┐
                    │      EventBus (事件总线)   │   ← 所有模块的核心通信层
                    └──────────┬───────────────┘
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
          v                    v                    v
   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
   │  数据存储引擎  │   │  因子计算引擎  │   │  执行引擎     │
   │  (Storage)   │   │  (Factor)     │   │  (Execution)  │
   └──────┬───────┘   └──────┬───────┘   └──────┬───────┘
          │                  │                   │
          │    数据供给       │   因子结果          │   订单执行
          │                  │                   │
          v                  v                   v
   ┌──────────────────────────────────────────────────────┐
   │                  风控引擎 (Risk)                       │
   │   pre_order_check ←── 执行引擎下单前                   │
   │   periodic_check  ←── 定时巡逻                        │
   └───────────────────────────────────────────────────────┘
```

### 6.2 事件流转全景

```
  行情数据流（自上而下）：

  ┌────────────┐  MarketDataEvent  ┌────────────┐  FactorUpdateEvent  ┌──────────────┐
  │  数据接收    │───────────────> │  数据存储    │──────────────────> │  因子计算     │
  │  (网络层)   │                  │  (缓存更新)  │                     │  (增量更新)   │
  └────────────┘                  └────────────┘                     └──────┬───────┘
                                                                            │
                                                               FactorUpdateEvent
                                                                            │
  ┌────────────┐  TradeSignalEvent  ┌──────────────┐                       v
  │  策略引擎    │<─────────────── │  信号生成      │<────────────────────┘
  │  (Python)  │                   │  (因子触发)   │
  └─────┬──────┘                    └──────────────┘
        │ TradeSignalEvent
        v
  ┌──────────────┐  RiskCheckResult  ┌──────────────┐  OrderReportEvent  ┌────────────┐
  │  风控引擎     │─────────────────> │  执行引擎     │─────────────────> │  订单回报     │
  │  (预检)     │    pass/reject     │  (下单)      │                    │  (成交回报)  │
  └──────────────┘                    └──────┬───────┘                    └────────────┘
                                              │
                                    ┌─────────┴──────────┐
                                    │  券商网关            │
                                    │  (XTP/模拟)          │
                                    └────────────────────┘

  风控告警流（旁路）：

  ┌──────────────┐  RiskAlertEvent  ┌──────────────┐
  │  风控引擎     │───────────────> │  告警通知      │──> 飞书/WebSocket
  │  (定时巡检)   │                  │  (EventBus)   │
  └──────────────┘                  └──────────────┘
```

### 6.3 初始化与依赖注入

```cpp
// ===== 系统初始化顺序 =====

class QuantSystem {
public:
    void initialize() {
        // 1. 创建事件总线（最底层，无依赖）
        event_bus_ = std::make_shared<EventBus>();

        // 2. 创建数据存储引擎
        TimeSeriesStore::Options store_opts{
            .data_dir = "/data/quant",
            .memory_cache_mb = 2048,
            .compaction_threshold = 0.5,
        };
        store_ = std::make_shared<TimeSeriesStore>(store_opts);

        // 3. 创建因子计算引擎
        FactorRegistry::Options factor_opts{
            .compute_threads = 0,  // 自动检测
            .cache_memory_mb = 4096,
        };
        factor_scheduler_ = std::make_shared<FactorScheduler>(
            factor_registry_, store_, factor_opts);

        // 4. 创建风控引擎
        circuit_breaker_ = std::make_shared<CircuitBreaker>(
            CircuitBreaker::Config{});
        risk_engine_ = std::make_shared<RiskEngine>(
            circuit_breaker_, RiskEngine::Options{});

        // 5. 创建执行引擎
        broker_ = std::make_shared<XtpBrokerGateway>();
        order_mgr_ = std::make_shared<OrderManager>();
        algo_engine_ = std::make_shared<AlgoEngine>(order_mgr_, broker_);

        // 6. 订阅事件
        setup_event_subscriptions();
    }

private:
    void setup_event_subscriptions() {
        // 行情数据 → 数据存储
        event_bus_->subscribe(MarketDataEvent::kEventTypeId,
            &store_handler_);

        // 行情数据 → 因子增量更新
        event_bus_->subscribe(MarketDataEvent::kEventTypeId,
            &factor_handler_);

        // 交易信号 → 风控预检 → 执行引擎
        event_bus_->subscribe(TradeSignalEvent::kEventTypeId,
            &execution_handler_);

        // 订单回报 → 更新订单状态
        event_bus_->subscribe(OrderReportEvent::kEventTypeId,
            &order_report_handler_);

        // 风控告警 → 通知系统
        event_bus_->subscribe(RiskAlertEvent::kEventTypeId,
            &alert_handler_);
    }

    std::shared_ptr<EventBus>         event_bus_;
    std::shared_ptr<TimeSeriesStore>   store_;
    std::shared_ptr<FactorRegistry>    factor_registry_;
    std::shared_ptr<FactorScheduler>   factor_scheduler_;
    std::shared_ptr<RiskEngine>        risk_engine_;
    std::shared_ptr<CircuitBreaker>    circuit_breaker_;
    std::shared_ptr<OrderManager>      order_mgr_;
    std::shared_ptr<AlgoEngine>        algo_engine_;
    std::shared_ptr<IBrokerGateway>    broker_;
};
```

---

*文档结束。所有模块设计遵循 C++20 标准，使用 `concepts`、`span`、`atomic` 等现代特性，编译器要求 GCC 12+ 或 Clang 15+。*