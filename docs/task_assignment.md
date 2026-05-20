# 任务分配表 — Python DSL → IR → C++ 执行架构

> 日期: 2026-05-21
> 方案文档: strategy_ir_compilation_design.md

## Phase 0 — 基础组件 (并行开发)

| Task ID | 任务 | Agent | 依赖 | 文件 |
|---------|------|-------|------|------|
| #147 | P0-T1: Fix StorageEngine codec mismatch | storage-engine-expert | 无 | storage_engine.cc, storage_test.cc |
| #160 | P0-T2: Implement IR data structures | storage-engine-expert | #147 | ir/ir_graph.h/.cc |
| #161 | P0-T3: Implement OpRegistry | storage-engine-expert | #160 | factor/op_registry.h/.cc |
| #162 | P0-T5: Implement Python DSL v2 | go-backend-expert | 无 | strategy/dsl2.py |
| #159 | P0-T4: FactorDAG::from_graph() | storage-engine-expert | #160, #161 | factor/factor_dag.h/.cc |
| #163 | P0-T7: C++ signal handlers | storage-engine-expert | #159 | strategy/signal_handler.h/.cc |
| #158 | P0-T8: BacktestRunner + SimulatedBroker + Portfolio | storage-engine-expert | #159, #163 | backtest/, portfolio/ |
| #148 | P0-T6: Python IR compiler | go-backend-expert | #162 | strategy/ir_compiler.py |

## Phase 1 — 服务层

| Task ID | 任务 | Agent | 依赖 | 文件 |
|---------|------|-------|------|------|
| #151 | P1-T1: C++ resident service process | go-backend-expert | #159, #163, #158 | service/service_main.cc |
| #156 | P1-T2: DataIngestor network coroutines | storage-engine-expert | #151 | ingest/data_ingestor.cc |
| #149 | P1-T3: Strategy center (StrategyRegistry + StrategyEngine) | go-backend-expert | #159, #163 | strategy/strategy_registry.h/.cc |
| #150 | P1-T4: pybind11 bindings for IR/backtest/strategy | storage-engine-expert | #160, #158, #149 | pybind/py_ir.cc, py_backtest.cc |
| #154 | P1-T5: Strategy submission API | go-backend-expert | #149, #150 | api/strategy_api.py |

## Phase 2 — 前端闭环 + 集成测试

| Task ID | 任务 | Agent | 依赖 | 文件 |
|---------|------|-------|------|------|
| #152 | P2-T1: Frontend strategy pages | frontend-expert | #149, #154 | web/src/pages/ |
| #153 | P2-T2: End-to-end integration test | backend-testing-expert | #148, #159, #158 | tests/system/ |

## Phase 3 — 高级功能

| Task ID | 任务 | Agent | 依赖 | 文件 |
|---------|------|-------|------|------|
| #157 | P3-T1: Migrate IR to Protobuf | storage-engine-expert | #153 | protos/, ir/ |
| #155 | P3-T2: Live trading pipeline | go-backend-expert | #151, #155 | strategy_engine.cc |

## 执行流程

1. 总架构师 review 每个 task 完成情况
2. 完成后运行测试，确保通过
3. git commit + push 到远程仓库
4. 安排下一个 unblocked 的 task
5. 同一 Phase 内无依赖的 task 可并行分配

## Agent 分配原则

- **storage-engine-expert**: C++ 存储引擎、IR 加载、算子注册、FactorDAG、信号处理、回测引擎
- **go-backend-expert**: Python DSL、IR 编译器、常驻服务、策略中心、API、实盘管线
- **frontend-expert**: 前端策略页面、回测结果展示
- **backend-testing-expert**: 集成测试、性能基准测试
- **总架构师**: 任务分配、Review、推送、架构文档更新
