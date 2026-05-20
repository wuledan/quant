# 架构设计文档

## C++ 核心引擎

- [C++核心引擎架构](architecture_cpp_core.md) — 数据存储引擎、因子计算引擎、执行引擎、风控引擎、事件总线

## C++ 基础组件

- [C++基础组件架构](architecture_cpp_infra.md) — 线程池、网络层、配置管理、日志、pybind11互操作

## Python 层

- [Python层架构](architecture_python.md) — 数据采集、回测框架、策略研究、ML管道、API服务

## 前端与交互

- [前端与交互架构](architecture_frontend.md) — 前端页面、飞书机器人、API设计、系统交互流程

## 协程化改造

- [协程化改造设计](design_coroutine_refactor.md) — 全协程化改造方案

## 技术决策与性能

- [技术决策记录](technical_decisions.md) — AffinityBaton、CoMutex、io_uring 等关键技术选型
- [性能基准报告](benchmark_report.md) — Executor/DAG/协程性能测试数据