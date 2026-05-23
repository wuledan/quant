# 多级存储架构设计

> 日期: 2026-05-22 | 更新: 2026-05-23
> 状态: **Implemented** — 全部设计目标已实现
> 范围: 策略中心(etcd) + 行情/因子数据存储 + C++ 引擎数据交互

---

## 1. 设计目标

1. **策略中心独立化**: Python 编译 IR → 提交至 etcd → C++ 引擎 watch 拉取，解耦编译与执行
2. **数据规模支撑**: 5000+ A 股标的 × 10 年 × 多频率(日/1分/5分/15分/30分/60分)
3. **多级存储**: 远端(冷) → 本地磁盘(温) → 本地内存(热)，自动流转
4. **量化数据完备**: K 线、因子横截面、公司行为、复权因子、回测结果

---

## 2. 数据需求与规模估算

### 2.1 K 线数据

| 频率 | 每日根数 | 10 年总行数(5000 标的) | 原始大小 | 压缩后(Parquet Zstd) |
|------|---------|----------------------|---------|---------------------|
| 日线 | 1 | 1.2 亿 | 5.8 GB | ~0.8 GB |
| 60 分 | 4 | 4.9 亿 | 23 GB | ~3.2 GB |
| 30 分 | 8 | 9.8 亿 | 47 GB | ~6.4 GB |
| 15 分 | 16 | 19.6 亿 | 94 GB | ~12 GB |
| 5 分 | 48 | 58.8 亿 | 282 GB | ~32 GB |
| 1 分 | 240 | 29.4 亿 | 1413 GB | ~150 GB |

> 注: 压缩比取 Parquet Zstd 列式压缩经验值 ~7x (时间戳 delta + 价格 Gorilla-like + 整数列 RLE)

**全量 6 频率**: ~204 GB 压缩后。实际使用中，1 分/5 分数据通常只保留近 2-3 年。

### 2.2 因子横截面数据

| 类型 | 示例 | 行数(5000 标的 × 2450 日) | 压缩后 |
|------|------|--------------------------|--------|
| 基本面因子 | PE/PB/ROE/市值 | ~1225 万/因子 | ~50 MB/因子 |
| 技术因子 | 动量/波动率/换手率 | 同上 | 同上 |
| Alpha 因子 | WorldQuant #1-#101 | 同上 | 同上 |

500 个因子 × 50 MB = **~25 GB** 压缩后。

### 2.3 公司行为

除权除息/拆股/配股/停牌: 5000 标的 × ~20 事件/10 年 = **~10 万行**, < 10 MB。

### 2.4 策略 IR

~100 策略 × ~10 KB/策略 = **~1 MB**。

### 2.5 回测结果

每次回测: NAV 曲线(~5000 点 × 16B) + 指标 + 交易记录 ≈ 100 KB - 10 MB。
1000 次回测 ≈ **~1 GB**。

---

## 3. 三级存储架构

```
┌─────────────────────────────────────────────────────────────┐
│  Tier 0: 远端存储 (Cold — 历史归档 + 策略中心)              │
│                                                             │
│  ┌───────────────┐  ┌──────────────────┐  ┌──────────────┐ │
│  │     etcd       │  │  MinIO / S3      │  │  PostgreSQL  │ │
│  │  (策略中心)    │  │  (行情 + 因子    │  │  (元数据 +   │ │
│  │                │  │   归档 Parquet)  │  │   回测结果)  │ │
│  └────────────────┘  └──────────────────┘  └──────────────┘ │
└─────────────────────────────────────────────────────────────┘
         │ pull on startup / cache miss / watch
         ▼
┌─────────────────────────────────────────────────────────────┐
│  Tier 1: 本地磁盘 (Warm — 近期活跃数据)                     │
│                                                             │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Columnar Segment Files (.seg)                         │ │
│  │  + Segment Index (B+ tree: time range → file offset)  │ │
│  │  + Compaction Daemon (merge small → large segments)    │ │
│  │  + Upload Daemon (cold segments → MinIO async)         │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
         │ read-through on query
         ▼
┌─────────────────────────────────────────────────────────────┐
│  Tier 2: 本地内存 (Hot — 当前活跃标的)                      │
│                                                             │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Sharded LRU Cache (fixed budget, e.g. 512 MB)        │ │
│  │  + Read-through: miss → disk → fill cache              │ │
│  │  + Write-through: write → cache + async disk enqueue   │ │
│  │  + Periodic flush: dirty cache blocks → disk (30s)     │ │
│  │  + Eviction: LRU + proper memory accounting            │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. 远端存储选型

### 4.1 etcd — 策略中心

**选型理由**:
- 小对象 KV 存储 (IR JSON ~10KB, 元数据 ~1KB), 完全在 etcd 1.5MB value 限制内
- **Watch 机制**: C++ 引擎 watch 前缀变更, 实时感知策略增删改
- 强一致性 (Raft 协议), 适合策略注册这种不允许丢失的操作
- 分布式, 多引擎实例可同时 watch
- TTL 支持, 策略可设自动过期

**Key 布局**:
```
/quant/strategy/{id}/meta     → JSON {name, version, created_at, status, params}
/quant/strategy/{id}/ir       → JSON (StrategyGraph IR)
/quant/backtest/task/{id}     → JSON {strategy_id, symbol, start, end, status}
/quant/backtest/result/{id}   → JSON {metrics, nav_curve_s3_key}
```

**交互流程**:
1. Python 编译策略: `IRCompiler.compile(MACross)` → IR JSON
2. Python 写入 etcd: `PUT /quant/strategy/42/ir` + `PUT /quant/strategy/42/meta`
3. C++ 引擎 watch `/quant/strategy/` 前缀 → 收到 PUT 事件
4. 引擎读取 IR JSON → `StrategyGraph::load_from_json()` → 构建 FactorDAG → 注册到本地 StrategyEngine
5. 回测任务: 写入 `/quant/backtest/task/` → 引擎 watch 到 → 拉取策略 → 执行 → 结果写回 etcd

### 4.2 MinIO / S3 — 行情与因子归档

**选型理由**:
- S3 兼容协议, 可无缝切换 AWS S3 / 阿里云 OSS
- Parquet 列式格式: 压缩比优秀 (~7x Zstd), 支持 predicate pushdown (按时间范围裁剪)
- 按对象组织, 天然适合按标的/年分片
- 简单部署 (单二进制), 无需额外数据库服务

**对象布局**:
```
bucket: quant-kline
  /kline/{frequency}/{symbol}/{year}.parquet
  例: /kline/1min/600519.SH/2024.parquet
      /kline/day/600519.SH/2020.parquet

bucket: quant-factor
  /factor/{factor_name}/{year}.parquet
  例: /factor/pe/2024.parquet    (columns: date, symbol, value)
      /factor/momentum_20/2024.parquet

bucket: quant-corporate-actions
  /corporate-actions/{symbol}.parquet
```

**Parquet Schema (K 线)**:
```parquet
message Kline {
  required int64 timestamp;       // epoch microseconds
  required int32 open_price;      // ×10000 定点
  required int32 high_price;
  required int32 low_price;
  required int32 close_price;
  required int64 volume;
  required int64 amount;
  required int32 vwap;            // ×10000 定点 (修复一致性)
}
```

**Parquet Schema (因子横截面)**:
```parquet
message FactorCrossSection {
  required int64 date;            // epoch microseconds (当日 15:00)
  required binary symbol;        // UTF-8 string
  required double value;
}
```

**交互策略**:
- **启动预热**: 引擎启动时, 从 etcd 读取活跃标的列表, 从 MinIO 拉取近 N 天的 Parquet → 写入本地磁盘 → 填充缓存
- **回测加载**: 回测需要 [start, end] 时间范围 → 查本地磁盘 → 缺失的年份从 MinIO 拉取 → 写入本地磁盘 → 查询
- **增量上传**: 本地磁盘 compaction 后的冷段 (超过 N 天未访问) → 异步上传 MinIO → 本地可保留或删除

### 4.3 PostgreSQL — 元数据与回测结果

**选型理由**:
- 回测结果需要结构化查询 (按策略/标的/时间范围筛选)
- 元数据管理 (标的列表、数据源状态、数据覆盖范围)
- 事务支持, 适合回测结果的原子写入

**Schema**:
```sql
-- 标的注册表
CREATE TABLE symbols (
    symbol  VARCHAR(16) PRIMARY KEY,
    name    VARCHAR(64),
    market  VARCHAR(8),    -- SH/SZ
    status  VARCHAR(8),    -- ACTIVE/DELISTED/ST
    ipo_date DATE
);

-- 数据覆盖范围
CREATE TABLE data_coverage (
    symbol    VARCHAR(16),
    frequency VARCHAR(8),  -- day/1min/5min/...
    min_ts    BIGINT,
    max_ts    BIGINT,
    row_count BIGINT,
    PRIMARY KEY (symbol, frequency)
);

-- 回测结果
CREATE TABLE backtest_results (
    id            BIGSERIAL PRIMARY KEY,
    strategy_id   BIGINT,
    symbol        VARCHAR(16),
    start_ts      BIGINT,
    end_ts        BIGINT,
    initial_cash  DOUBLE PRECISION,
    total_return  DOUBLE PRECISION,
    annual_return DOUBLE PRECISION,
    max_drawdown  DOUBLE PRECISION,
    sharpe_ratio  DOUBLE PRECISION,
    total_trades  BIGINT,
    nav_s3_key    VARCHAR(256),  -- MinIO key for NAV curve
    created_at    TIMESTAMP DEFAULT NOW()
);
```

---

## 5. 本地磁盘层改造

### 5.1 现有问题

1. 无段索引: `list_segments()` 遍历目录 O(n), 百万文件不可接受
2. 无段合并: 小段持续累积, 查询需打开大量文件
3. 无 WAL: 崩溃后实时数据丢失
4. 单行写入低效: 每行创建 8 个 ColumnBlock, 压缩开销大

### 5.2 改造方案

**段索引 (Segment Index)**:
- 启动时扫描 `data_dir/*.seg` → 构建 `unordered_map<CacheKey, vector<SegmentMeta>>`
- `SegmentMeta`: {file_path, field, codec, row_count, min_ts, max_ts, file_offset}
- 查询时直接命中索引, 跳过目录遍历

**WAL (Write-Ahead Log)**:
- `store_kline()` 单行写入 → 先追加 WAL (append-only file, fsync)
- 后台线程批量从 WAL 读取 → 构建 ColumnBlock → 写入缓存 + 段文件
- 崩溃恢复: 重放 WAL 中未提交的记录

**写入缓冲 (Write Buffer)**:
- 单行 `store_kline()` 不再立即创建 ColumnBlock
- 攒批到 WriteBuffer (每 8192 行或每 5 秒 flush 一次)
- 批量写入时才创建压缩的 ColumnBlock → 高效

**段合并 (Compaction Daemon)**:
- 后台线程定期扫描: 同标的同频率的小段 (< 4096 行) → 合并为大段
- 合并后原子替换: 新段写入 → 索引更新 → 旧段删除
- 触发条件: 小段数量 > 阈值 或 碎片率 > 50%

**冷段上传 (Upload Daemon)**:
- 超过 N 天未访问的段 → 转换为 Parquet → 上传 MinIO
- 上传成功后, 本地段可保留 (LRU 淘汰) 或删除 (节省磁盘)

---

## 6. 本地内存层改造

### 6.1 Bug 修复

**缓存淘汰失效**:
```cpp
// 修复前: shard.memory_used 在 erase 后不变, freed 永远为 0
size_t before = shard.memory_used;
shard.columns.erase(it);
size_t freed = before - shard.memory_used;  // freed = 0!

// 修复后: 先计算条目内存, 再 erase, 再减计数器
size_t entry_mem = 0;
for (const auto& block : it->second) {
    entry_mem += block_memory(block);
}
shard.columns.erase(it);
shard.last_access.erase(key);
shard.memory_used -= entry_mem;
total_memory_.fetch_sub(entry_mem, std::memory_order_relaxed);
```

**vwap 一致性**: 统一为 int32 ×10000 定点编码, 与 OHLC 价格字段一致。

### 6.2 策略增强

**Read-Through**: 查询缓存 miss → 查磁盘 → 结果回填缓存
**Write-Through**: 写入同时更新缓存 + 异步写磁盘队列
**Periodic Flush**: 可配置间隔 (默认 30s), dirty 缓存块刷盘

---

## 7. C++ 引擎与 etcd 交互设计

### 7.1 Watch 流程

```
C++ Engine 启动
  │
  ├─ 1. 连接 etcd, 读取 /quant/strategy/* 全量 → 注册到本地 StrategyEngine
  ├─ 2. Watch /quant/strategy/ 前缀 (长连接)
  ├─ 3. Watch /quant/backtest/task/ 前缀
  │
  ├─ Watch 事件处理:
  │   ├─ PUT /quant/strategy/{id}/ir    → 加载 IR → 构建 FactorDAG → 激活策略
  │   ├─ PUT /quant/strategy/{id}/meta  → 更新元数据 (状态/参数)
  │   ├─ DELETE /quant/strategy/{id}/*  → 停止策略 → 释放资源
  │   └─ PUT /quant/backtest/task/{id}  → 拉取策略 → 执行回测 → 写结果
  │
  └─ 4. 连接 MinIO, 预热活跃标的近 N 天数据
```

### 7.2 etcd C++ 客户端

使用 [etcd-cpp-apiv3](https://github.com/etcd-cpp/etcd-cpp-apiv3) 库:
- 基于 gRPC + protobuf, 与 etcd v3 API 通信
- 支持 watch (基于 HTTP/2 stream, 事件驱动)
- CMake FetchContent 集成

### 7.3 Python 端

```python
import etcd3

client = etcd3.client()

# 编译策略 → 写入 etcd
def submit_strategy(strategy_cls):
    compiler = IRCompiler()
    ir_json = compiler.compile(strategy_cls)
    strategy_id = generate_id()
    client.put(f"/quant/strategy/{strategy_id}/ir", ir_json)
    client.put(f"/quant/strategy/{strategy_id}/meta", json.dumps({
        "name": strategy_cls._strategy_name,
        "version": 1,
        "created_at": now(),
        "status": "DRAFT",
    }))
    return strategy_id

# 提交回测任务
def submit_backtest(strategy_id, symbol, start, end):
    task_id = generate_id()
    client.put(f"/quant/backtest/task/{task_id}", json.dumps({
        "strategy_id": strategy_id,
        "symbol": symbol,
        "start_time": start,
        "end_time": end,
        "status": "PENDING",
    }))
    return task_id
```

---

## 8. 数据生命周期与流转规则

> 核心原则：**数据只从热层向冷层沉淀，淘汰方向与同步方向正交**。
> 实时数据淘汰至冷层时必须同步至远端（防止丢失）；历史数据从远端加载后淘汰时不同步（远端已有）。

### 8.1 实时行情：拉取 → 加工 → 沉淀

```
DataIngestor (引擎主动拉取行情)
  │
  ├─ 1. 原始 KlineRow 进入内存
  │     → WAL (fsync, 崩溃安全)
  │     → WriteBuffer (攒批, 8192行/5s flush)
  │
  ├─ 2. 内存加工（引擎侧，不落盘）
  │     → FactorDAG 实时计算因子值
  │     → 信号检测 (CROSS_ABOVE 等)
  │     → 交易信号生成 → OrderSignalHandler
  │     → 加工后的因子/信号数据也进入 LRU Cache
  │
  ├─ 3. 批量写入内存缓存
  │     → ColumnBlock (压缩) → LRU Cache append
  │     → 异步磁盘写入队列
  │
  └─ 4. 逐步沉淀（热 → 温 → 冷）
        → [每 30s] 缓存脏块 flush → 本地 .seg 文件
        → [每天] 冷段 → Parquet → 异步上传 MinIO
        → 上传成功后本地 .seg 可保留或删除
```

**关键设计点**：
- **引擎自行拉取**：DataIngestor 是引擎内部的协程，主动连接行情源 TCP/WS 拉取，非外部推送
- **内存优先加工**：原始行情进入内存后立即用于因子计算和信号检测，加工在 LRU Cache 中完成
- **异步远端同步**：WriteBuffer flush 到缓存/磁盘后，有独立的 Upload Daemon 将冷段异步上传至 MinIO，不阻塞实时路径
- **因子数据同步流**：实时计算的因子横截面同样走 WriteBuffer → 缓存 → 磁盘 → MinIO 路径

### 8.2 数据淘汰规则：沉淀 vs 加载

**场景 A：实时数据的 LRU 淘汰（沉淀）**

实时数据从内存淘汰时，必须确保已同步至下一层：

```
实时 K 线 / 因子
  → 内存 LRU 满时淘汰
  → 检查：是否已 flush 到磁盘？
    → 否：先 flush 再淘汰
    → 是：直接淘汰
  → 磁盘冷段淘汰（超过 N 天未访问）
  → 检查：是否已上传 MinIO？
    → 否：先上传再删除本地段
    → 是：直接删除本地段
```

**规则**：实时数据的淘汰 = 沉淀。每一层淘汰前必须确保下一层已有数据，**淘汰与同步方向一致**。

**场景 B：历史数据的 LRU 淘汰（不同步）**

从远端加载的历史数据，淘汰时不需要回写：

```
回测需要历史数据 [2020-01-01, 2023-12-31]
  → 内存缓存 miss
  → 本地磁盘 miss（或不足）
  → MinIO 拉取 Parquet → 写入本地 .seg → 填充缓存
  → 回测计算使用
  → 回测完成后数据逐渐变为冷数据
  → 内存 LRU 淘汰：直接淘汰（磁盘有副本）
  → 磁盘 LRU 淘汰：直接删除（MinIO 有副本）
```

**规则**：从远端加载的数据淘汰 = 纯淘汰。远端已是权威来源，**淘汰时不同步**。

### 8.3 淘汰方向矩阵

| 数据来源 | 内存→磁盘 | 磁盘→MinIO | 说明 |
|---------|----------|-----------|------|
| 实时拉取 | 先 flush 再淘汰 | 先上传再删除 | 沉淀：淘汰=同步至下一层 |
| 远端加载 | 直接淘汰 | 直接删除 | 纯淘汰：远端已有，无需回写 |

实现上，每个 Cache 条目 / 磁盘段需要标记来源：

```cpp
enum class DataSource : uint8_t {
    kRealtimeIngest = 0,   // 引擎实时拉取，淘汰时需同步
    kRemoteLoad     = 1,   // 从远端加载，淘汰时不同步
};

struct CacheEntryMeta {
    DataSource source;
    bool disk_synced = false;    // 是否已 flush 到磁盘（realtime 用）
    bool remote_synced = false;  // 是否已上传远端（realtime 用）
};
```

淘汰逻辑：

```cpp
void evict_entry(const CacheKey& key, CacheEntry& entry) {
    if (entry.meta.source == DataSource::kRealtimeIngest) {
        // 沉淀模式：确保下一层有数据
        if (!entry.meta.disk_synced) {
            flush_to_disk(key, entry);  // 先写磁盘
        }
        if (!entry.meta.remote_synced) {
            upload_daemon_.enqueue(key); // 异步上传（不阻塞淘汰）
        }
    }
    // 远端加载的数据：直接淘汰，不回写
    remove_from_cache(key);
}
```

### 8.4 按需远端加载（Read-Through）

引擎在计算过程中，数据不足时自动从远端补齐：

```
BacktestRunner.query_kline(symbol, type, field, range)
  │
  ├─ 1. 内存缓存查询
  │     → hit: 返回
  │     → partial: 记录缺失的时间范围
  │     → miss: 全部需要从下层加载
  │
  ├─ 2. 缓存 miss → 本地磁盘查询
  │     → hit: 解压 → 回填缓存（标记 kRemoteLoad）→ 返回
  │     → partial/miss: 记录缺失范围
  │
  └─ 3. 磁盘 miss → MinIO 拉取
        → 下载对应年份 Parquet
        → 转换为 .seg 写入本地磁盘
        → 填充缓存（标记 kRemoteLoad）
        → 返回数据
```

**预取优化**：回测请求 [2020, 2023] 时，如果 2020.parquet 已在 MinIO，可预判 2021-2023 也需要，异步预取。

---

## 9. 数据流总结

### 9.1 写入路径 (实时 K 线)

```
DataIngestor (TCP/WS 行情源)
  → KlineRow
  → WAL (fsync, 崩溃安全)
  → WriteBuffer (攒批)
  → [每 8192 行 或 每 5s]
  → ColumnBlock (压缩)
  → 内存缓存 (append, source=kRealtimeIngest)
  → 异步磁盘写入队列
  → [每 30s] 缓存脏块 flush → .seg 文件 (disk_synced=true)
  → [每天] 冷段 → Parquet → MinIO 上传 (remote_synced=true)
```

### 9.2 读取路径 (回测查询)

```
BacktestRunner.query_kline(symbol, type, field, range)
  → 内存缓存查询 (LRU hit)
  → [miss] → 段索引查找 → 本地磁盘 .seg 读取 → 解压 → 回填缓存 (source=kRemoteLoad)
  → [miss] → MinIO 拉取 Parquet → 写入本地 .seg → 填充缓存 (source=kRemoteLoad) → 返回
```

### 9.3 策略变更路径

```
Python: submit_strategy() → etcd PUT
C++ Engine: etcd watch event → load IR → build FactorDAG → activate
回测触发: etcd PUT /quant/backtest/task/ → engine watch → execute → result → etcd PUT
```

---

## 10. 实施优先级

| 优先级 | 任务 | 工作量 |
|--------|------|--------|
| P0 | 修复缓存淘汰 Bug | 0.5d |
| P0 | 修复 vwap 编码一致性 | 0.5d |
| P0 | 实现 WAL + WriteBuffer + 后台 flush | 2d |
| P0 | etcd 策略中心客户端 + watch | 2d |
| P1 | 段索引 (启动时构建, 消除目录遍历) | 1d |
| P1 | 段合并 Compaction Daemon | 2d |
| P1 | 多频率 K 线支持 | 1d |
| P1 | MinIO 客户端 + Parquet 格式 | 3d |
| P2 | PostgreSQL 元数据 + 回测结果 | 2d |
| P2 | 因子横截面存储 | 2d |
| P2 | 公司行为 + 复权因子 | 1d |
| P3 | 冷段上传 Daemon | 2d |
| P3 | mmap 零拷贝读盘 | 已移除（见附录 A） |

---

## 附录 A: 设计讨论记录

### A.1 mmap 零拷贝读盘 — 移除决策 (2026-05-22)

**结论**: 从计划中移除，不再实施。

**理由**:
1. 热数据已在 LRU 缓存，高频访问不走磁盘，mmap 无用武之地
2. 冷数据瓶颈是解压（CPU），不是 I/O。ColumnBlock 压缩后读盘很快，mmap 省的 memcpy 在解压开销前可忽略
3. mmap 收益场景是"大文件随机访问少量字节"（如 B+ 树点查），我们的查询是时间范围顺序扫描，read() + OS readahead 已够好
4. 增加复杂度：SIGBUS 处理、TLB 压力、大页配置，收益不值得

**真正能提升性能的**: 段索引、Compaction、Read-Through + 预取。

### A.2 数据生命周期规则 — 淘汰方向与同步方向正交 (2026-05-22)

**核心原则**: 数据只从热层向冷层沉淀，淘汰方向与同步方向正交。

- **实时数据淘汰 = 沉淀**: 每一层淘汰前必须确保下一层已有数据，淘汰与同步方向一致
- **远端加载数据淘汰 = 纯淘汰**: 远端已是权威来源，淘汰时不同步

实现通过 `DataSource` enum + `CacheEntryMeta` 结构标记每个缓存条目的来源和同步状态。

### A.3 协程友好的同步原语 — 自研决策 (2026-05-22)

**背景**: 全系统基于 folly::coro 协程调度，使用 `AffinityBaton`（线程亲和 baton）和 `WorkStealingExecutor`（线程亲和调度器）。阻塞操作会阻塞整个线程而非单个协程，破坏调度公平性。

**问题**: `folly::coro::Mutex` 和 `folly::coro::SharedMutex` 不满足线程亲和性要求：
- `folly::coro::Mutex`: 无线程亲和性，唤醒不经过 `WorkStealingExecutor::add_to_worker()`
- `folly::coro::SharedMutex`: 源码明确声明 "The locks do not have thread affinity"；内部使用 `folly::SpinLock` 阻塞自旋

**决策**: 自研 `AffinityMutex` 和 `AffinitySharedMutex`：
- 状态通过 `std::atomic` 管理（无 SpinLock）
- 等待者通过 intrusive linked list + `worker_id` 记录
- 唤醒时通过 `WorkStealingExecutor::add_to_worker()` 路由到正确线程
- 接口与 `folly::coro::Mutex` / `folly::coro::SharedMutex` 兼容

**实现状态**:
- `AffinityMutex`: ✅ 已实现并提交（单原子状态字编码锁标志+等待者指针）
- `AffinitySharedMutex`: 🚧 开发中

### A.4 实施进展 (2026-05-23) — 全部完成

| 任务 | 状态 | 提交 |
|------|------|------|
| 缓存淘汰 Bug 修复 | ✅ | d86a41b |
| vwap 编码一致性修复 | ✅ | d86a41b |
| WAL + WriteBuffer 实现 | ✅ | d86a41b |
| WriteBuffer 集成 StorageEngine | ✅ | b686e1d |
| DataSource 追踪 | ✅ | b686e1d |
| AffinityMutex | ✅ | d86a41b |
| AffinitySharedMutex | ✅ | d86a41b |
| WriteBuffer/WAL 协程迁移 | ✅ | b686e1d |
| TimeSeriesCache 协程迁移 | ✅ | d86a41b |
| Read-Through 查询路径 | ✅ | 49152ac |
| 缓存淘汰与预算控制 | ✅ | b686e1d |
| 磁盘持久化 | ✅ | b686e1d |
| Compaction Daemon | ✅ | 85940ba |
| Periodic Dirty Flush (30s) | ✅ | 61fab61 |
| MinIO/S3 客户端 (RemoteStorage) | ✅ | ce23f8c |
| PostgreSQL 元数据 | ✅ | 291a8e2 |
| 因子横截面存储 | ✅ | 291a8e2 |
| 公司行为 + 复权因子 | ✅ | 291a8e2 |
| 冷段上传 Daemon | ✅ | 5159254 |
| DiskPersistence 异步 I/O (io_uring) | ✅ | ce23f8c |
| Python DSL + IRCompiler | ✅ | 5159254 (py/) |
| EventBus 协程迁移 | ✅ | 272c242 |
| 13 组件阻塞原语替换 | ✅ | 272c242 |
| 多频率 DataInitializer | ✅ | 77a4082 |
| 测试恢复 (ColumnBlock/Disk/WAL/Buffer) | ✅ | 170bf49 |

