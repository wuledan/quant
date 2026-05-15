# Quant Invest - 量化投资系统

面向中国A股市场的量化投资系统，采用 C++20（高性能层）+ Python（策略研究层）技术栈。

## 系统架构

```
┌─────────────────────────────────────────────────┐
│               用户交互层                          │
│   Web前端 (Next.js)  │  飞书机器人                 │
├─────────────────────────────────────────────────┤
│               API网关层 (Python/FastAPI)          │
│   REST + WebSocket  │  JWT认证  │  飞书OAuth      │
├─────────────────────────────────────────────────┤
│               业务服务层 (Python)                  │
│   策略服务 │ 回测服务 │ 因子服务 │ 风控服务 │ 信号服务 │
├─────────────────────────────────────────────────┤
│               核心引擎层 (C++20)                   │
│   数据存储 │ 因子计算 │ 执行引擎 │ 风控引擎 │ 事件总线 │
├─────────────────────────────────────────────────┤
│               数据层                              │
│   PostgreSQL │ Redis │ ClickHouse │ MinIO         │
└─────────────────────────────────────────────────┘
```

## 技术栈

| 层级 | 技术选型 |
|------|----------|
| 核心引擎 | C++20, Google C++编码规范, SIMD/AVX2, pybind11 |
| 策略研究 | Python 3.12+, FastAPI, NumPy/Pandas |
| 前端 | Next.js 14, TypeScript, TailwindCSS, ECharts/D3 |
| 消息通信 | Redis (Streams), EventBus (C++ Pub/Sub) |
| 数据存储 | PostgreSQL, ClickHouse, 自研时序引擎 |
| 交互 | 飞书开放平台 (Bot + 卡片消息) |

## 策略方向

- 多因子选股（Alpha因子挖掘、IC/IR分析、风险模型）
- 统计套利（配对交易、板块轮动）
- 事件驱动（财报公告、监管政策、新闻情绪NLP）
- 机器学习/AI（XGBoost/LSTM/Transformer）
- 宏观策略（经济指标、政策事件、地缘风险）

## 文档

| 文档 | 说明 |
|------|------|
| [C++核心引擎架构](docs/architecture_cpp_core.md) | 数据存储、因子计算、执行引擎、风控引擎、事件总线 |
| [C++基础组件架构](docs/architecture_cpp_infra.md) | 线程池、网络层、配置管理、日志、pybind11互操作 |
| [Python层架构](docs/architecture_python.md) | 数据采集、回测框架、策略研究、ML管道、API服务 |
| [前端与交互架构](docs/architecture_frontend.md) | 前端页面、飞书机器人、API设计、系统交互流程 |

## 项目结构

```
quant_invest/
├── src/
│   ├── storage/          # 数据存储引擎 (C++)
│   ├── factor/           # 因子计算引擎 (C++)
│   ├── execution/        # 执行引擎 (C++)
│   ├── risk/             # 风控引擎 (C++)
│   ├── event/            # 事件总线 (C++)
│   ├── network/          # 网络层 (C++)
│   ├── common/           # 公共基础库 (C++)
│   └── pybind/           # Python绑定 (C++)
├── python/
│   ├── data/             # 数据采集与管线
│   ├── backtest/         # 回测框架
│   ├── strategy/         # 策略研究框架
│   ├── ml/               # ML/AI管道
│   ├── api/              # FastAPI服务
│   └── feishu/           # 飞书机器人
├── frontend/             # Next.js前端
├── docs/                 # 架构设计文档
└── tests/                # 测试
```

## 开发环境

- 编译器：GCC 12+ 或 Clang 15+
- Python：3.12+
- Node.js：18+
- 构建：CMake 3.25+, Conan/Ninja
- 包管理：uv/poetry (Python), pnpm (Node.js)

## License

Private - All Rights Reserved