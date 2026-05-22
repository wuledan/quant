# 讨论记录 2026-05-22

## 参与者
- 用户
- Claude (总架构师 / C++ 后端 + 存储专家)

## 议题

### 1. 策略提交流程重新设计

**问题**: 当前 Python 编译 IR 后通过 HTTP 上传到 C++ 服务本地 SQLite, C++ 与策略存储紧耦合, 无法支持多引擎实例拉取策略。

**决策**: 
- 使用 **etcd** 作为策略中心存储
- Python 编译 IR → 直接写入 etcd (`/quant/strategy/{id}/ir` + `/quant/strategy/{id}/meta`)
- C++ 引擎 **watch** etcd 前缀 `/quant/strategy/`, 实时感知策略变更
- 回测任务也通过 etcd 下发: `/quant/backtest/task/{id}`
- 引擎 watch 到回测任务 → 拉取策略 IR → 执行 → 结果写回 etcd

**理由**:
- etcd watch 机制天然适配 "引擎拉取" 模式, 无需轮询
- Raft 强一致性保证策略不丢失
- 分布式: 多引擎实例可同时 watch 同一个 etcd
- 小对象 KV (IR JSON ~10KB) 完全在 etcd 1.5MB value 限制内

### 2. 存储层 Bug 确认与修复

**发现的关键 Bug**:

1. **缓存淘汰完全失效** (`time_series_cache.cc`): 
   - `evict()` 中 `shard.memory_used` 在 `erase` 后不变, `freed` 始终为 0
   - 结果: 内存只增不减, 缓存淘汰无效, 长时间运行会 OOM
   - **修复方案**: 先计算条目内存再 erase, 手动减少 `shard.memory_used` 和 `total_memory_`

2. **实时数据无持久化**: `store_kline()` 单行写入只写缓存, 不触发落盘
   - 结果: 进程崩溃即丢失所有实时数据
   - **修复方案**: 引入 WAL + WriteBuffer 攒批 + 后台定期 flush

3. **vwap 编码不一致**: 价格字段 int32×10000 定点, vwap 按 int64 原生存储
   - **修复方案**: 统一为 int32×10000 定点

### 3. 多级存储架构设计

**需求**: 量化交易需要支撑 5000+ 标的 × 10 年 × 多频率数据, 并支持因子横截面、公司行为、回测结果等数据类型。

**决策**: 三级存储架构

| 层级 | 引擎 | 数据 | 交互策略 |
|------|------|------|---------|
| Tier 0 远端 | etcd (策略) + MinIO/S3 (行情Parquet) + PostgreSQL (元数据/结果) | 冷数据: 历史归档 | 启动预热 / cache miss pull / watch |
| Tier 1 本地磁盘 | Columnar .seg + 段索引 + WAL + Compaction | 温数据: 近期活跃 | read-through / write-through |
| Tier 2 本地内存 | LRU 缓存 (分片, 固定预算) | 热数据: 当前活跃标的 | read-through / write-through / 定期 flush |

**远端存储选型理由**:
- **etcd**: 小 KV + watch + 强一致, 完美匹配策略中心
- **MinIO/S3 + Parquet**: 列式压缩 (~7x Zstd), S3 兼容可切换云厂商, 按标的/年分片天然适配
- **PostgreSQL**: 结构化查询回测结果, 事务支持, 元数据管理

**详细设计**: 参见 `docs/multi_tier_storage_design.md`

### 4. 数据规模评估

| 数据类型 | 全量规模 (5000 标的 × 10 年) |
|---------|----------------------------|
| K 线 (6 频率) | ~204 GB 压缩后 (Parquet Zstd) |
| 因子横截面 (500 因子) | ~25 GB 压缩后 |
| 公司行为 | < 10 MB |
| 策略 IR | ~1 MB |
| 回测结果 (1000 次) | ~1 GB |

---

## 行动项

| 优先级 | 行动 | 负责 |
|--------|------|------|
| P0 | 修复缓存淘汰 Bug | Claude |
| P0 | 修复 vwap 编码一致性 | Claude |
| P0 | 实现 WAL + WriteBuffer + 后台 flush | Claude |
| P0 | etcd 策略中心集成 + watch | Claude |
| P1 | 段索引 + 段合并 | Claude |
| P1 | 多频率 K 线支持 | Claude |
| P1 | MinIO + Parquet 客户端 | Claude |
| P2 | PostgreSQL 元数据 | Claude |
| P2 | 因子横截面存储 | Claude |
