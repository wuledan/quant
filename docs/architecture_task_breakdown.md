# 架构重设计 — 组件级任务分解

> 日期: 2026-05-20
> 关联: architecture_redesign.md

## P0: C++ StorageEngine 接入数据采集

### P0-T1: pybind11 模块编译验证 ✅ (2026-05-20)
- **组件**: `cpp/quant/pybind/` → `_quant_core.so`
- **任务**: 确认 CMake 编译通过, `import _quant_core` 可用, StorageEngine/FactorDAG/OrderManager 全部可实例化
- **产出**: 编译脚本, 验证测试 (30/30 pass), 设计文档 `docs/p0-t1-pybind-module-design.md`
- **负责**: storage-engine-expert
- **依赖**: 无
- **备注**: 修复了 Result<T> 包装、shm.get() size 字段、枚举值对齐

### P0-T2: StorageEngine 数据初始化
- **组件**: `StorageEngine` + `DiskPersistence`
- **任务**: 启动时从现有 Parquet 文件加载历史数据到 StorageEngine 列存; 若无 Parquet 则从 Yahoo Finance 全量拉取并写入
- **产出**: `DataInitializer` 类, 支持批量导入
- **负责**: storage-engine-expert
- **依赖**: P0-T1

### P0-T3: C++ DataIngestor 协程采集
- **组件**: `SchedulerService` + `StorageEngine` + `EventBus`
- **任务**: 实现 C++ 协程定时拉取行情数据 (通过 Python callback 调 yfinance), 结果写入 StorageEngine 并 publish KlineEvent
- **产出**: `DataIngestor` 类, 注册到 SchedulerService 的 cron 任务
- **负责**: storage-engine-expert
- **依赖**: P0-T2

### P0-T4: Python 调度器迁移到 StorageEngine
- **组件**: `data/scheduler.py` → 调用 `_quant_core.StorageEngine`
- **任务**: Python DataSchedulerService.get_daily_data() 改为调用 C++ StorageEngine.query_kline(), 返回 numpy array; 保留 Python 层做 fallback
- **产出**: 修改后的 scheduler.py
- **负责**: go-backend-expert
- **依赖**: P0-T3

### P0-T5: P0 集成测试
- **组件**: 全链路
- **任务**: 验证: 启动 → 数据加载 → 增量采集 → query_kline 返回正确数据 → Python API 可用
- **产出**: 集成测试脚本
- **负责**: backend-testing-expert
- **依赖**: P0-T4

---

## P1: Python 策略热加载 + DAG 编译器

### P1-T1: 策略声明式 DSL 设计 ✅ (2026-05-20)
- **组件**: `strategy/dsl.py`
- **任务**: 定义 Factor / cross_above / cross_below / Strategy 基类, 策略用装饰器 + 类属性声明因子和信号
- **产出**: `strategy/dsl.py`, 示例策略
- **负责**: go-backend-expert
- **依赖**: 无

### P1-T2: 策略编译器 (Python AST → FactorDAG)
- **组件**: `strategy/compiler.py`
- **任务**: 解析策略类定义, 提取 Factor 节点和依赖关系, 注册到 C++ FactorRegistry, 调用 FactorDAG.build() 构建执行图
- **产出**: `StrategyCompiler` 类
- **负责**: go-backend-expert
- **依赖**: P1-T1, P0-T1

### P1-T3: 文件监视器 + 热重载
- **组件**: `strategy/hot_reload.py`
- **任务**: 使用 watchfiles 监视策略目录, 检测 .py 文件变更 → 重新 import → 重新编译 → 增量更新 DAG (不影响运行中策略)
- **产出**: `StrategyWatcher` 类, 集成到 FastAPI lifespan
- **负责**: go-backend-expert
- **依赖**: P1-T2

### P1-T4: 策略注册表重构
- **组件**: `strategy/registry.py`
- **任务**: 支持 DSL 策略和传统 on_bar 策略共存; 注册表记录策略 → DAG 映射; 提供 list/get/reload API
- **产出**: 修改后的 registry.py
- **负责**: go-backend-expert
- **依赖**: P1-T2

### P1-T5: P1 集成测试
- **组件**: 策略热加载全链路
- **任务**: 验证: 编写策略 → 自动注册 → 修改策略文件 → 5秒内自动重载 → DAG 更新 → 回测使用新策略
- **产出**: 测试脚本
- **负责**: backend-testing-expert
- **依赖**: P1-T4

---

## P2: 回测走 StorageEngine + FactorDAG

### P2-T1: BacktestRunner 重构
- **组件**: `backtest/runner.py`
- **任务**: 新的 BacktestRunner: 从 StorageEngine.query_kline() 读数据 (零拷贝 numpy), 替代 DailyDataHandler; 支持传统策略和 DSL 策略
- **产出**: `BacktestRunner` 类
- **负责**: go-backend-expert
- **依赖**: P0-T4, P1-T2

### P2-T2: 因子计算走 C++ FactorDAG
- **组件**: `backtest/factor_engine.py`
- **任务**: 回测中每根 K 线触发 FactorDAG.compute(), 因子值传给策略; C++ 并行计算, Python 获取结果
- **产出**: `FactorEngineBridge` 类
- **负责**: storage-engine-expert
- **依赖**: P0-T1, P1-T2

### P2-T3: 订单模拟走 C++ OrderManager
- **组件**: `backtest/order_bridge.py`
- **任务**: 策略产生信号 → C++ OrderManager 创建订单 → SimulatedBroker 模拟成交 → FillEvent 返回 Python
- **产出**: `OrderBridge` 类
- **负责**: storage-engine-expert
- **依赖**: P0-T1

### P2-T4: 回测结果对比验证
- **组件**: 回测引擎
- **任务**: 同一策略同一参数, 新旧引擎回测结果对比 (净值曲线误差 < 0.1%); 性能基准测试
- **产出**: 对比测试 + 性能报告
- **负责**: backend-testing-expert
- **依赖**: P2-T3

---

## P3: C++ WebSocketServer 接入

### P3-T1: WebSocketServer 启动与 EventBus 桥接
- **组件**: `network/websocket_server.h` + `event/event_bus.h`
- **任务**: 启动 WebSocketServer :8080; EventBus 订阅 KlineEvent/FactorUpdateEvent/OrderReportEvent/RiskAlertEvent → 序列化为 JSON → broadcast
- **产出**: `WsEventBridge` 类
- **负责**: storage-engine-expert
- **依赖**: P0-T3

### P3-T2: 前端连接迁移
- **组件**: `web/src/`
- **任务**: 前端 WebSocket 连接从 FastAPI :8000/ws 切换到 C++ :8080; 解析新消息格式; 保留 HTTP API 走 FastAPI
- **产出**: 修改后的前端 WS 连接
- **负责**: frontend-expert
- **依赖**: P3-T1

### P3-T3: P3 集成测试
- **组件**: WS 推送全链路
- **任务**: 验证: C++ WS 启动 → 前端连接 → 数据推送 → 延迟 < 10ms
- **产出**: 测试脚本
- **负责**: backend-testing-expert
- **依赖**: P3-T2

---

## P4: 交易时段 EventBus 全链路

### P4-T1: Kline → Factor → Signal 全链路
- **组件**: EventBus + FactorDAG + Strategy
- **任务**: KlineEvent 发布 → FactorDAG 计算 → FactorUpdateEvent → 策略 on_signal → SignalEvent → OrderManager; 端到端延迟 < 1ms
- **产出**: 全链路集成
- **负责**: storage-engine-expert
- **依赖**: P2-T3, P3-T1

### P4-T2: 风控引擎接入
- **组件**: `risk/risk_engine.h`
- **任务**: OrderManager 下单前 → RiskEngine.check() → 通过则提交, 否则 publish RiskAlertEvent
- **产出**: 风控拦截逻辑
- **负责**: storage-engine-expert
- **依赖**: P4-T1

### P4-T3: P4 端到端测试
- **组件**: 全系统
- **任务**: 模拟交易时段: 行情到达 → 因子计算 → 策略信号 → 下单 → 风控 → 成交 → WS 推送 → 前端显示
- **产出**: 端到端测试
- **负责**: backend-testing-expert
- **依赖**: P4-T2

---

## 依赖关系图

```
P0-T1 ─→ P0-T2 ─→ P0-T3 ─→ P0-T4 ─→ P0-T5
  │                              │
  │                              ↓
  ├─→ P1-T2 ←─ P1-T1            P2-T1 ─→ P2-T2 ─→ P2-T3 ─→ P2-T4
  │     │                        ↑         ↑         ↑
  │     ├─→ P1-T3 ─→ P1-T5     │         │         │
  │     └─→ P1-T4 ─→ P1-T5     │         │         │
  │                               │         │         │
  └─→ P3-T1 ─→ P3-T2 ─→ P3-T3  │         │         │
        ↑                         │         │         │
        │                         └─────────┘         │
        │                                               │
        └─→ P4-T1 ─→ P4-T2 ─→ P4-T3 ←───────────────┘
              ↑
              └── P2-T3 + P3-T1
```

## Agent 任务分配

| Agent | 任务 |
|-------|------|
| storage-engine-expert | P0-T1, P0-T2, P0-T3, P2-T2, P2-T3, P3-T1, P4-T1, P4-T2 |
| go-backend-expert | P0-T4, P1-T1, P1-T2, P1-T3, P1-T4, P2-T1 |
| frontend-expert | P3-T2 |
| backend-testing-expert | P0-T5, P1-T5, P2-T4, P3-T3, P4-T3 |
| 总架构师 | 任务分配, Review, 推送, 架构文档更新 |
