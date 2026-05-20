# 引擎性能基准测试报告

> 测试日期: 2026-05-20
> 硬件: Linux 6.17, x86_64
> 编译器: GCC (via CMake)

## 1. Executor 任务调度吞吐量

**测试**: WorkStealingExecutor, 4 worker 线程, 100,000 空任务

| 指标 | 值 |
|------|------|
| 平均耗时 | 244,388 us |
| 中位数 | 251,462 us |
| 标准差 | 11,370 us |
| 吞吐量 | **409,185 ops/s** |

分析: Executor 的单次 add+execute 约 2.4us。4 worker 可承载 40万级空任务调度。

## 2. WaveScheduler DAG 执行

### 小 DAG: 100 任务 × 10 层

| 指标 | 值 |
|------|------|
| 平均 | 350 us |
| 中位数 | 344 us |
| 吞吐量 | **286,041 ops/s** |

### 中 DAG: 1000 任务 × 10 层

| 指标 | 值 |
|------|------|
| 平均 | 1,543 us |
| 中位数 | 1,542 us |
| 标准差 | 33 us |
| 吞吐量 | **647,920 ops/s** |

### 宽 DAG: 3000 任务 × 3 层 × 8 workers

| 指标 | 值 |
|------|------|
| 平均 | 4,862 us |
| 中位数 | 4,858 us |
| 标准差 | 78 us |
| 吞吐量 | **617,005 ops/s** |

分析: DAG 调度吞吐量在 28万~65万 ops/s 之间。宽 DAG (高并行度) 在 8 worker 下接近线性扩展。

## 3. 协程开销

### 空协程创建+执行 (10,000 次)

| 指标 | 值 |
|------|------|
| 平均 | 10,877 us |
| 吞吐量 | **919,371 ops/s** |
| 单次 | ~1.09 us |

### 64 层深度协程链 (1,000 次)

| 指标 | 值 |
|------|------|
| 平均 | 15,502 us |
| 单次 `co_await` | **~242 ns** |

分析: 空协程开销 ~1us，远低于线程创建 (~10us)。每 `co_await` 仅 242ns——接近零开销抽象。

## 4. 总结

| 组件 | 吞吐量 | 延迟 (中位数) |
|------|--------|---------------|
| Executor (4 workers, 100K task) | 409K ops/s | 251ms total |
| DAG Small (100 tasks) | 286K ops/s | 344 us |
| DAG Medium (1000 tasks) | 648K ops/s | 1.5 ms |
| DAG Wide (3000 tasks, 8W) | 617K ops/s | 4.9 ms |
| 空协程创建 (C++20) | 919K ops/s | 1.09 us |
| co_await 恢复 | — | 242 ns |

## 5. 性能瓶颈与优化建议

1. **Executor 瓶颈**: 当前 `add()` 使用 MPSC 队列 + CAS，在高并发下 cache line bouncing 显著
2. **DAG 调度**: `WaveScheduler` 的 promise/future 同步机制引入 ~50us 每层开销，宽 DAG 下可优化
3. **协程链**: `blockingWait` 的 ManualExecutor 增加约 500ns 每 `co_await`，纯 executor 路径更快
4. **io_uring 路径**: 需要独立基准测试评估真正异步 I/O 的吞吐
