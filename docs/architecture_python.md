# Python层架构设计文档

> 版本：v0.1 | 更新日期：2026-05-15
> 定位：Python层作为策略研究、数据采集、回测验证、ML/AI管道和交互接口的完整解决方案，与C++/Rust高性能层紧密协作。

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [项目目录结构](#2-项目目录结构)
3. [数据采集模块](#3-数据采集模块)
4. [回测框架](#4-回测框架)
5. [策略研究框架](#5-策略研究框架)
6. [ML/AI管道](#6-mlai管道)
7. [API服务层](#7-api服务层)
8. [飞书机器人交互](#8-飞书机器人交互)
9. [项目依赖管理](#9-项目依赖管理)
10. [模块间交互总览](#10-模块间交互总览)
11. [与C++层交互接口](#11-与c层交互接口)

---

## 1. 整体架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                     Python 策略研究层                            │
│                                                                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────┐    │
│  │ 数据采集  │  │ 回测框架  │  │ 策略研究  │  │  ML/AI 管道   │    │
│  │  Module  │  │  Module  │  │  Module  │  │   Module     │    │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬───────┘    │
│       │             │              │                │            │
│       └─────────────┴──────┬───────┴────────────────┘            │
│                            │                                     │
│  ┌────────────────────────┴──────────────────────────┐          │
│  │              因子API / cFFI / pybind11              │          │
│  └────────────────────────┬──────────────────────────┘          │
│                            │                                     │
│  ┌────────────────────────┴──────────────────────────┐          │
│  │              API服务层 (FastAPI)                      │          │
│  └────────────────────────┬──────────────────────────┘          │
│                            │                                     │
│  ┌────────────────────────┴──────────────────────────┐          │
│  │              飞书机器人交互                           │          │
│  └─────────────────────────────────────────────────────┘          │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
                            │
                            ▼ (pybind11 / cFFI / 共享内存 / Arrow IPC)
┌─────────────────────────────────────────────────────────────────┐
│              C++/Rust 高性能层（因子引擎 / 执行引擎）              │
└─────────────────────────────────────────────────────────────────┘
```

**核心设计原则：**

- **研究优先**：Python层以策略研发效率为第一优先级
- **性能委托**：计算密集型操作委托给C++/Rust层
- **数据驱动**：所有模块通过统一数据总线交互，避免点对点耦合
- **渐进式**：模块可独立交付，Phase 1 即可产出价值

---

## 2. 项目目录结构

```
quant_invest/
├── py/                              # Python层根目录
│   ├── pyproject.toml               # 项目配置 & 依赖
│   ├── README.md
│   ├── src/
│   │   └── quant_invest/            # 主包
│   │       ├── __init__.py
│   │       ├── config/              # 全局配置
│   │       │   ├── __init__.py
│   │       │   ├── settings.py      # Pydantic Settings
│   │       │   └── logging_conf.py
│   │       ├── data/                # 数据采集模块
│   │       │   ├── __init__.py
│   │       │   ├── providers/       # 数据源适配器
│   │       │   │   ├── __init__.py
│   │       │   │   ├── base.py
│   │       │   │   ├── akshare_provider.py
│   │       │   │   ├── tushare_provider.py
│   │       │   │   └── wind_provider.py
│   │       │   ├── collectors/      # 各类数据采集器
│   │       │   │   ├── __init__.py
│   │       │   │   ├── daily_bar.py
│   │       │   │   ├── minute_bar.py
│   │       │   │   ├── tick_data.py
│   │       │   │   ├── financial.py
│   │       │   │   ├── index_constituent.py
│   │       │   │   └── industry_classification.py
│   │       │   ├── quality/         # 数据质量校验
│   │       │   │   ├── __init__.py
│   │       │   │   ├── validator.py
│   │       │   │   ├── anomaly_detector.py
│   │       │   │   └── repair.py
│   │       │   ├── scheduler.py     # 增量更新调度
│   │       │   └── cache.py         # 本地缓存管理
│   │       ├── backtest/            # 回测框架
│   │       │   ├── __init__.py
│   │       │   ├── engine.py        # 事件驱动引擎
│   │       │   ├── events.py        # 事件定义
│   │       │   ├── broker.py        # 模拟经纪商
│   │       │   ├── slippage.py      # 滑点模型
│   │       │   ├── commission.py    # 手续费模型
│   │       │   ├── restrictions.py  # 涨跌停限制
│   │       │   ├── portfolio.py     # 持仓管理
│   │       │   ├── performance.py   # 绩效分析
│   │       │   └── data_handler.py  # 数据馈送
│   │       ├── strategy/            # 策略研究框架
│   │       │   ├── __init__.py
│   │       │   ├── base.py          # 策略基类
│   │       │   ├── context.py       # 策略上下文
│   │       │   ├── signal.py        # 信号生成与组合
│   │       │   ├── factor_api.py    # 因子API（桥接C++）
│   │       │   ├── optimizer.py     # 参数优化框架
│   │       │   └── registry.py     # 策略注册中心
│   │       ├── ml/                  # ML/AI管道
│   │       │   ├── __init__.py
│   │       │   ├── feature/         # 特征工程
│   │       │   │   ├── __init__.py
│   │       │   │   ├── builder.py
│   │       │   │   └── transformer.py
│   │       │   ├── models/         # 模型定义
│   │       │   │   ├── __init__.py
│   │       │   │   ├── xgboost_model.py
│   │       │   │   ├── lstm_model.py
│   │       │   │   └── transformer_model.py
│   │       │   ├── pipeline.py     # 训练管线
│   │       │   ├── evaluation.py    # 模型评估
│   │       │   └── versioning.py   # 模型版本管理
│   │       ├── api/                 # API服务层
│   │       │   ├── __init__.py
│   │       │   ├── app.py           # FastAPI应用
│   │       │   ├── dependencies.py  # 依赖注入
│   │       │   ├── auth.py          # 认证与权限
│   │       │   ├── routes/
│   │       │   │   ├── __init__.py
│   │       │   │   ├── strategy.py
│   │       │   │   ├── backtest.py
│   │       │   │   ├── portfolio.py
│   │       │   │   ├── factor.py
│   │       │   │   └── system.py
│   │       │   ├── schemas.py       # Pydantic模型
│   │       │   └── ws.py            # WebSocket管理
│   │       └── bot/                 # 飞书机器人
│   │           ├── __init__.py
│   │           ├── app.py           # 飞书事件订阅
│   │           ├── commands/        # 指令处理
│   │           │   ├── __init__.py
│   │           │   ├── portfolio.py
│   │           │   ├── performance.py
│   │           │   └── strategy_ctrl.py
│   │           ├── pusher.py        # 信号推送
│   │           └── templates.py     # 消息模板
│   ├── tests/                       # 测试
│   │   ├── conftest.py
│   │   ├── data/
│   │   ├── backtest/
│   │   ├── strategy/
│   │   ├── ml/
│   │   ├── api/
│   │   └── bot/
│   └── scripts/                      # 脚本
│       ├── fetch_data.py
│       ├── run_backtest.py
│       └── train_model.py
├── cpp/                              # C++高性能层（独立目录）
│   └── ...
└── docs/
    └── architecture_python.md        # 本文档
```

---

## 3. 数据采集模块

### 3.1 核心类/接口定义

```python
# data/providers/base.py

from abc import ABC, abstractmethod
from datetime import date, datetime
from typing import Optional
import pandas as pd
from enum import Enum


class DataFreq(str, Enum):
    """数据频率"""
    TICK = "tick"
    MIN_1 = "1min"
    MIN_5 = "5min"
    MIN_15 = "15min"
    MIN_30 = "30min"
    MIN_60 = "60min"
    DAILY = "daily"
    WEEKLY = "weekly"
    MONTHLY = "monthly"


class AdjustMethod(str, Enum):
    """复权方式"""
    NONE = "none"          # 不复权
    FORWARD = "forward"    # 前复权
    BACKWARD = "backward"  # 后复权


class DataProvider(ABC):
    """数据源提供者基类

    所有数据源（akshare/tushare/wind）均需实现此接口。
    统一的接口使得上层代码不依赖具体数据源。
    """

    @property
    @abstractmethod
    def name(self) -> str:
        """数据源名称，如 'akshare', 'tushare', 'wind'"""
        ...

    @abstractmethod
    def get_daily_bars(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        """获取日行情数据

        Returns:
            DataFrame with columns: [open, high, low, close, volume, amount, turnover]
            Index: DatetimeIndex (trade_date)
        """
        ...

    @abstractmethod
    def get_minute_bars(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        """获取分钟线数据"""
        ...

    @abstractmethod
    def get_tick_data(
        self,
        symbol: str,
        trade_date: date,
    ) -> pd.DataFrame:
        """获取Tick级别数据"""
        ...

    @abstractmethod
    def get_financial_data(
        self,
        symbol: str,
        report_type: str = "income",
        start_date: Optional[date] = None,
        end_date: Optional[date] = None,
    ) -> pd.DataFrame:
        """获取财务数据（利润表/资产负债表/现金流量表）"""
        ...

    @abstractmethod
    def get_index_constituents(
        self,
        index_symbol: str,
        trade_date: Optional[date] = None,
    ) -> pd.DataFrame:
        """获取指数成分股"""
        ...

    @abstractmethod
    def get_industry_classification(
        self,
        symbol: Optional[str] = None,
        level: str = "L1",
    ) -> pd.DataFrame:
        """获取行业分类（申万/中信行业分类）"""
        ...

    @abstractmethod
    def get_trade_calendar(
        self,
        start_date: date,
        end_date: date,
    ) -> list[date]:
        """获取交易日历"""
        ...

    @abstractmethod
    def health_check(self) -> bool:
        """数据源健康检查"""
        ...
```

```python
# data/quality/validator.py

from dataclasses import dataclass, field
from datetime import date, datetime
from enum import Enum
import pandas as pd
from typing import Optional


class QualityLevel(str, Enum):
    PASS = "pass"
    WARNING = "warning"
    ERROR = "error"


@dataclass
class QualityReport:
    """数据质量报告"""
    level: QualityLevel
    checks: list[dict] = field(default_factory=list)
    # 每项: {"name": str, "status": QualityLevel, "detail": str}
    missing_dates: list[date] = field(default_factory=list)
    anomalous_rows: list[int] = field(default_factory=list)
    summary: str = ""


class DataValidator:
    """数据质量校验器

    校验规则：
    1. 完整性：交易日是否有缺失
    2. 一致性：OHLC关系（high >= low, high >= open/close 等）
    3. 连续性：检测异常跳空
    4. 合理性：涨跌幅是否超出涨跌停限制
    5. 缺失值：NaN比例检查
    """

    def __init__(self, config: Optional[dict] = None):
        self.config = config or {}

    def validate(
        self,
        df: pd.DataFrame,
        symbol: str,
        freq: DataFreq,
        start_date: date,
        end_date: date,
    ) -> QualityReport:
        ...

    def validate_consistency(self, df: pd.DataFrame) -> list[dict]:
        """OHLC关系一致性校验"""
        ...

    def validate_completeness(
        self,
        df: pd.DataFrame,
        freq: DataFreq,
        start_date: date,
        end_date: date,
    ) -> list[dict]:
        """数据完整性校验"""
        ...

    def validate_reasonability(self, df: pd.DataFrame) -> list[dict]:
        """涨跌停合理性校验"""
        ...


class AnomalyDetector:
    """异常数据检测与修复

    使用统计方法检测异常值（Z-score / IQR / IsolationForest）
    提供自动修复策略（插值 / 前值填充 / 标记删除）
    """

    def detect(
        self,
        df: pd.DataFrame,
        columns: list[str],
        method: str = "zscore",
        threshold: float = 3.0,
    ) -> pd.Series:
        """检测异常行索引"""
        ...

    def repair(
        self,
        df: pd.DataFrame,
        anomalies: pd.Series,
        strategy: str = "interpolate",
    ) -> pd.DataFrame:
        """修复异常数据"""
        ...
```

```python
# data/scheduler.py

from dataclasses import dataclass
from datetime import date, time, datetime
from enum import Enum
from typing import Optional
import pandas as pd


class UpdateMode(str, Enum):
    FULL = "full"              # 全量更新
    INCREMENTAL = "incremental"  # 增量更新
    REPAIR = "repair"          # 修补缺失


@dataclass
class UpdateTask:
    """增量更新任务"""
    provider: str            # 数据源
    data_type: str           # 数据类型: daily, minute, financial, ...
    symbols: list[str]       # 股票代码列表
    mode: UpdateMode
    start_date: Optional[date] = None
    end_date: Optional[date] = None
    priority: int = 0        # 调度优先级


class DataScheduler:
    """增量更新调度器

    策略：
    1. 每日定时触发收盘数据更新（15:30后）
    2. 根据本地已有数据的最新日期自动计算增量区间
    3. 支持断点续传：记录每个symbol的最新数据日期
    4. 失败重试：指数退避重试，超过阈值告警
    5. 并发控制：按数据源限制并发数，避免触发限频
    """

    def __init__(self, config: Optional[dict] = None):
        self._providers: dict[str, DataProvider] = {}
        self._progress: dict[str, date] = {}   # {symbol: last_updated_date}

    def register_provider(self, name: str, provider: DataProvider) -> None:
        ...

    async def run_task(self, task: UpdateTask) -> pd.DataFrame:
        ...

    async def run_daily_update(self) -> dict[str, bool]:
        """执行每日增量更新"""
        ...

    def get_progress(self, symbol: str) -> Optional[date]:
        """获取某标的最新数据日期"""
        ...

    def _calculate_incremental_range(
        self, symbol: str, data_type: str
    ) -> tuple[date, date]:
        """根据进度记录计算增量区间"""
        ...
```

### 3.2 数据流描述

```
[外部数据源] ──(HTTP API)──> [DataProvider]
                                  │
                      ┌───────────┴───────────┐
                      ▼                       ▼
               [DataCollector]          [DataCollector]
              (daily/minute/             (financial/
               tick)                      index/industry)
                      │                       │
                      └───────────┬───────────┘
                                  ▼
                          [DataValidator]  ──> QualityReport
                                  │
                                  ▼
                        [DataScheduler] ──> 写入存储层
                                │         (Arrow IPC / Parquet / 时序DB)
                                │
                          [CacheManager] ──> 本地Parquet缓存
```

### 3.3 模块间交互

- **数据采集 → 回测框架**：`DataHandler` 通过 `DataScheduler` 获取历史行情数据
- **数据采集 → 策略研究**：策略通过 `context.data()` 访问数据，底层走 `DataProvider`
- **数据采集 → ML管道**：特征工程通过 `DataProvider` 获取原始数据用于特征构建
- **数据采集 → 存储**：通过 Arrow IPC / Parquet 格式写入，与C++层共享数据格式

---

## 4. 回测框架

### 4.1 核心类/接口定义

```python
# backtest/events.py

from dataclasses import dataclass
from datetime import datetime
from enum import Enum, auto
from typing import Optional, Any


class EventType(Enum):
    MARKET = auto()        # 市场数据事件
    SIGNAL = auto()        # 交易信号事件
    ORDER = auto()        # 订单事件
    FILL = auto()         # 成交事件
    TIMER = auto()        # 定时事件


@dataclass
class Event:
    """事件基类"""
    type: EventType
    timestamp: datetime


@dataclass
class MarketEvent(Event):
    """市场数据事件：新的K线/Tick到达"""
    type: EventType = EventType.MARKET
    symbol: str = ""
    bar_data: Optional[dict] = None   # OHLCV数据


@dataclass
class SignalEvent(Event):
    """策略信号事件"""
    type: EventType = EventType.SIGNAL
    symbol: str = ""
    direction: str = ""     # "LONG" / "SHORT" / "EXIT"
    strength: float = 1.0  # 信号强度 [0, 1]
    price: float = 0.0
    reason: str = ""


@dataclass
class OrderEvent(Event):
    """订单事件"""
    type: EventType = EventType.ORDER
    symbol: str = ""
    order_type: str = "MARKET"  # MARKET / LIMIT / STOP
    quantity: int = 0
    direction: str = "BUY"     # BUY / SELL
    price: float = 0.0          # 限价单价格
    order_id: str = ""


@dataclass
class FillEvent(Event):
    """成交事件"""
    type: EventType = EventType.FILL
    symbol: str = ""
    direction: str = "BUY"
    quantity: int = 0
    fill_price: float = 0.0
    commission: float = 0.0
    slippage: float = 0.0
    order_id: str = ""
```

```python
# backtest/engine.py

from datetime import datetime, date
from typing import Optional, Callable
from collections import defaultdict
import pandas as pd

from .events import Event, EventType, MarketEvent
from .data_handler import DataHandler
from .broker import SimulatedBroker
from .portfolio import Portfolio
from ..strategy.base import StrategyBase


class BacktestEngine:
    """事件驱动回测引擎

    核心循环：
    1. DataHandler 产生 MarketEvent
    2. MarketEvent 分发给所有 Strategy
    3. Strategy 产生 SignalEvent
    4. SignalEvent 分发给 Portfolio 生成 OrderEvent
    5. OrderEvent 分发给 SimulatedBroker 执行
    6. FillEvent 返回 Portfolio 更新持仓
    7. 重复直至数据耗尽
    """

    def __init__(
        self,
        data_handler: DataHandler,
        broker: SimulatedBroker,
        portfolio: Portfolio,
        strategy: StrategyBase,
        initial_cash: float = 1_000_000.0,
        benchmark: str = "000300.SH",
    ):
        self.data_handler = data_handler
        self.broker = broker
        self.portfolio = portfolio
        self.strategy = strategy
        self.initial_cash = initial_cash
        self.benchmark = benchmark
        self._event_queue: list[Event] = []
        self._continue_backtest: bool = True

    def run(
        self,
        start_date: date,
        end_date: date,
        frequency: str = "daily",
    ) -> "BacktestResult":
        """执行回测主循环"""
        self._initialize(start_date)
        while self._continue_backtest:
            self._tick()
        return self._generate_result()

    def _tick(self) -> None:
        """单步事件循环"""
        # 1. 获取下一个市场数据
        market_event = self.data_handler.next_bar()
        if market_event is None:
            self._continue_backtest = False
            return

        # 2. 策略处理市场数据，生成信号
        signals = self.strategy.on_bar(
            market_event.bar_data,
            self.portfolio.current_positions,
        )

        # 3. 组合管理：信号 → 目标仓位 → 订单
        orders = self.portfolio.generate_orders(signals, market_event)

        # 4. 模拟执行
        fills = self.broker.execute_orders(orders, market_event)

        # 5. 更新持仓与净值
        self.portfolio.update_from_fills(fills, market_event.timestamp)
        self.portfolio.record_snapshot(market_event.timestamp)

    def _initialize(self, start_date: date) -> None:
        """初始化回测状态"""
        self.portfolio.initialize(self.initial_cash)
        self.strategy.on_init()

    def _generate_result(self) -> "BacktestResult":
        """生成回测结果报告"""
        ...
```

```python
# backtest/broker.py

from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime
from typing import Optional
import pandas as pd

from .events import OrderEvent, FillEvent, MarketEvent


@dataclass
class CommissionConfig:
    """手续费配置（A股规则）"""
    stamp_tax_rate: float = 0.001      # 印花税：卖出千分之一
    commission_rate: float = 0.00025   # 券商佣金：万2.5
    min_commission: float = 5.0        # 最低佣金5元
    slippage_rate: float = 0.001       # 滑点率
    slippage_model: str = "percentage" # "percentage" / "fixed" / "volume_weighted"


@dataclass
class RestrictionConfig:
    """涨跌停限制配置"""
    limit_up_ratio: float = 0.1        # 涨停比例 10%
    limit_down_ratio: float = -0.1     # 跌停比例 -10%
    check_limit: bool = True           # 是否检查涨跌停
    block_buy_at_limit_up: bool = True        # 涨停不买入
    block_sell_at_limit_down: bool = True     # 跌停不卖出


class SimulatedBroker:
    """模拟经纪商

    模拟真实交易的各个方面：
    - 滑点：按比例/固定/成交量加权
    - 手续费：印花税(卖)+券商佣金(双向)
    - 涨跌停：涨停无法买入，跌停无法卖出
    - 成交量限制：订单量不超过当前bar成交量的可占比例
    """

    def __init__(
        self,
        commission_config: Optional[CommissionConfig] = None,
        restriction_config: Optional[RestrictionConfig] = None,
    ):
        self.commission_config = commission_config or CommissionConfig()
        self.restriction_config = restriction_config or RestrictionConfig()

    def execute_orders(
        self,
        orders: list[OrderEvent],
        market_event: MarketEvent,
    ) -> list[FillEvent]:
        """执行订单列表，返回成交结果"""
        fills = []
        for order in orders:
            fill = self._execute_single(order, market_event)
            if fill is not None:
                fills.append(fill)
        return fills

    def _execute_single(
        self,
        order: OrderEvent,
        market_event: MarketEvent,
    ) -> Optional[FillEvent]:
        """执行单个订单"""
        # 1. 计算滑点
        fill_price = self._apply_slippage(order, market_event)

        # 2. 检查涨跌停
        if self._is_restricted(order, market_event):
            return None

        # 3. 计算手续费
        commission = self._calculate_commission(order, fill_price)

        return FillEvent(
            timestamp=market_event.timestamp,
            symbol=order.symbol,
            direction=order.direction,
            quantity=order.quantity,
            fill_price=fill_price,
            commission=commission,
            slippage=abs(fill_price - order.price) if order.price else 0.0,
            order_id=order.order_id,
        )

    def _apply_slippage(self, order: OrderEvent, market_event: MarketEvent) -> float:
        """应用滑点模型"""
        ...

    def _is_restricted(self, order: OrderEvent, market_event: MarketEvent) -> bool:
        """检查涨跌停限制"""
        ...

    def _calculate_commission(self, order: OrderEvent, fill_price: float) -> float:
        """计算手续费（印花税 + 券商佣金）"""
        ...
```

```python
# backtest/performance.py

from dataclasses import dataclass
import pandas as pd
import numpy as np


@dataclass
class PerformanceMetrics:
    """绩效分析指标"""
    # 收益指标
    total_return: float             # 总收益率
    annual_return: float            # 年化收益率
    benchmark_return: float         # 基准收益率
    excess_return: float            # 超额收益率（alpha）

    # 风险指标
    annual_volatility: float        # 年化波动率
    max_drawdown: float             # 最大回撤
    max_drawdown_duration: int      # 最大回撤持续天数
    sharpe_ratio: float             # 夏普比率
    sortino_ratio: float            # 索提诺比率
    calmar_ratio: float             # 卡玛比率

    # 交易指标
    total_trades: int               # 总交易次数
    win_rate: float                 # 胜率
    profit_loss_ratio: float        # 盈亏比
    turnover_rate: float            # 换手率
    avg_holding_days: float         # 平均持仓天数

    # 绩效归因
    monthly_returns: pd.Series      # 月度收益率
    sector_exposure: pd.DataFrame   # 行业暴露


class PerformanceAnalyzer:
    """绩效分析器"""

    def analyze(
        self,
        portfolio_snapshots: pd.DataFrame,  # daily portfolio values
        benchmark_series: pd.Series,
        risk_free_rate: float = 0.03,
    ) -> PerformanceMetrics:
        """计算完整绩效指标"""
        ...

    @staticmethod
    def calc_max_drawdown(equity_curve: pd.Series) -> tuple[float, int]:
        """计算最大回撤及持续天数"""
        ...

    @staticmethod
    def calc_sharpe_ratio(
        returns: pd.Series,
        risk_free_rate: float = 0.03,
        periods: int = 252,
    ) -> float:
        """计算夏普比率"""
        ...

    @staticmethod
    def calc_turnover(portfolio_snapshots: pd.DataFrame) -> float:
        """计算换手率"""
        ...

    def generate_report(
        self,
        metrics: PerformanceMetrics,
        output_path: str = "",
    ) -> str:
        """生成绩效报告（文本/HTML）"""
        ...
```

```python
# backtest/data_handler.py

from abc import ABC, abstractmethod
from datetime import date
import pandas as pd

from .events import MarketEvent


class DataHandler(ABC):
    """数据处理器基类"""

    @abstractmethod
    def next_bar(self) -> Optional[MarketEvent]:
        """获取下一根K线"""
        ...

    @abstractmethod
    def current_bar(self, symbol: str) -> Optional[dict]:
        """获取当前K线数据"""
        ...

    @abstractmethod
    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取历史N根K线"""
        ...


class DailyDataHandler(DataHandler):
    """日线数据处理器"""

    def __init__(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        data_provider: "DataProvider",
    ):
        self._symbols = symbols
        self._data: dict[str, pd.DataFrame] = {}
        self._current_idx: int = 0
        self._load_data(start_date, end_date, data_provider)

    def _load_data(self, start_date, end_date, data_provider):
        """预加载所有数据"""
        ...


class MinuteDataHandler(DataHandler):
    """分钟线数据处理器

    与日线不同，分钟线数据量较大，采用流式加载：
    - 启动时仅加载元数据和索引
    - 按需从Parquet文件流式读取
    - 内存中仅保留滑动窗口数据
    """

    def __init__(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        data_provider: "DataProvider",
        window: int = 200,
    ):
        self._symbols = symbols
        self._window = window
        self._buffer: dict[str, pd.DataFrame] = {}
        ...
```

```python
# backtest/portfolio.py

from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional
import pandas as pd

from .events import SignalEvent, OrderEvent, FillEvent


@dataclass
class Position:
    """单个持仓"""
    symbol: str
    quantity: int = 0
    avg_price: float = 0.0
    market_value: float = 0.0
    unrealized_pnl: float = 0.0


class Portfolio:
    """组合管理器

    负责：
    1. 根据策略信号计算目标仓位
    2. 生成调仓订单（目标仓位 - 当前仓位）
    3. 维护持仓和净值记录
    """

    def __init__(self):
        self.initial_cash: float = 0.0
        self.cash: float = 0.0
        self.positions: dict[str, Position] = {}
        self._nav_history: list[dict] = []

    def initialize(self, initial_cash: float) -> None:
        ...

    @property
    def current_positions(self) -> dict[str, Position]:
        ...

    @property
    def total_value(self) -> float:
        """总资产 = 现金 + 持仓市值"""
        ...

    def generate_orders(
        self,
        signals: list[SignalEvent],
        market_event: MarketEvent,
    ) -> list[OrderEvent]:
        """根据信号和当前持仓生成订单"""
        ...

    def update_from_fills(self, fills: list[FillEvent], timestamp: datetime) -> None:
        """根据成交结果更新持仓"""
        ...

    def record_snapshot(self, timestamp: datetime) -> None:
        """记录当前组合快照（用于后续绩效分析）"""
        ...

    def get_nav_dataframe(self) -> pd.DataFrame:
        """获取净值曲线DataFrame"""
        ...
```

### 4.2 数据流描述

```
[DataHandler] ──MarketEvent──> [BacktestEngine]
                                   │
                     ┌─────────────┴─────────────┐
                     ▼                           │
                [Strategy]                        │
                     │                             │
              SignalEvent                          │
                     │                             │
                     ▼                             │
               [Portfolio] ──OrderEvent──> [SimulatedBroker]
                                                  │
                                            FillEvent
                                                  │
                                                  ▼
                                           [Portfolio更新持仓]
                                                  │
                                                  ▼
                                         [NAV快照记录]
                                                  │
                                          ┌───────┴───────┐
                                          ▼               ▼
                                   [绩效分析器]    [报告生成器]
```

### 4.3 模块间交互

- **回测 → 数据采集**：`DataHandler` 通过 `DataProvider` 获取历史数据
- **回测 → 策略**：引擎调用 `strategy.on_bar()` 驱动策略逻辑
- **回测 → C++因子引擎**：策略可调用 `factor_api` 获取因子值
- **回测 → API服务**：通过 FastAPI 暴露回测触发和结果查询接口

---

## 5. 策略研究框架

### 5.1 核心类/接口定义

```python
# strategy/base.py

from abc import ABC, abstractmethod
from datetime import date, datetime
from typing import Optional, Any
from dataclasses import dataclass, field

import pandas as pd


@dataclass
class StrategyParams:
    """策略参数基类

    所有策略的参数均应继承此类，支持：
    - 类型检查（通过 dataclass field 类型注解）
    - 参数优化范围定义（通过 metadata 标记搜索空间）
    - 序列化/反序列化
    """
    pass


class StrategyBase(ABC):
    """策略基类

    生命周期：
    __init__ → on_init → (on_bar 循环) → on_finish

    子类需实现：
    - on_init(): 初始化，计算预加载因子等
    - on_bar(): 每根K线触发的信号生成逻辑
    - on_finish(): 收尾，输出统计等
    """

    def __init__(self, params: Optional[StrategyParams] = None):
        self.params = params or StrategyParams()
        self._context: Optional["StrategyContext"] = None

    @abstractmethod
    def on_init(self) -> None:
        """策略初始化

        用途：
        - 预加载数据
        - 计算初始因子值
        - 设置策略状态
        """
        ...

    @abstractmethod
    def on_bar(self, bar_data: dict, positions: dict) -> list["SignalEvent"]:
        """每根K线触发

        Args:
            bar_data: 当前bar数据 {symbol: {open, high, low, close, volume, ...}}
            positions: 当前持仓 {symbol: Position}

        Returns:
            信号列表
        """
        ...

    def on_finish(self) -> None:
        """策略结束（可选重写）"""
        pass

    def on_order_filled(self, fill: "FillEvent") -> None:
        """订单成交回调（可选重写）"""
        pass

    @property
    def context(self) -> "StrategyContext":
        """策略上下文：访问数据、因子、日志等"""
        assert self._context is not None, "Context not initialized"
        return self._context

    def set_context(self, context: "StrategyContext") -> None:
        self._context = context


class SignalGenerator:
    """信号生成与组合

    支持多个子策略的信号合并：
    1. 简单合并：所有子策略信号的并集
    2. 投票合并：超过N个子策略同意才产生信号
    3. 加权合并：按子策略历史表现加权
    4. 分层合并：先用因子信号过滤，再用择时信号确认
    """

    def __init__(self, method: str = "union"):
        self.method = method
        self._sub_strategies: list[StrategyBase] = []
        self._weights: list[float] = []

    def add_strategy(self, strategy: StrategyBase, weight: float = 1.0) -> None:
        ...

    def combine(self, bar_data: dict, positions: dict) -> list["SignalEvent"]:
        """合并多个子策略信号"""
        ...
```

```python
# strategy/context.py

from datetime import date
from typing import Optional
import pandas as pd


class StrategyContext:
    """策略上下文

    为策略提供统一的运行时环境：
    - 数据访问
    - 因子计算（调用C++引擎）
    - 日志记录
    - 参数管理
    """

    def __init__(
        self,
        data_handler: "DataHandler",
        factor_api: Optional["FactorAPI"] = None,
    ):
        self._data_handler = data_handler
        self._factor_api = factor_api
        self._logs: list[dict] = []

    # ── 数据访问 ──

    def current_bar(self, symbol: str) -> Optional[dict]:
        """获取当前K线数据"""
        ...

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取最近N根K线"""
        return self._data_handler.history(symbol, bars)

    def get_factor(
        self,
        name: str,
        symbols: list[str],
        date: date,
    ) -> pd.Series:
        """调用C++因子引擎计算因子

        Args:
            name: 因子名称，如 "momentum_20d"
            symbols: 标的列表
            date: 计算日期

        Returns:
            pd.Series, index=symbol, values=factor_value
        """
        if self._factor_api is None:
            raise RuntimeError("Factor API not available")
        return self._factor_api.calculate(name, symbols, date)

    # ── 日志 ──

    def log(self, message: str, level: str = "INFO") -> None:
        """记录策略日志"""
        self._logs.append({"message": message, "level": level})

    # ── 其他工具 ──

    def get_trading_calendar(self, start: date, end: date) -> list[date]:
        """获取交易日历"""
        ...
```

```python
# strategy/factor_api.py

from datetime import date
from typing import Optional, Union
import pandas as pd


class FactorAPI:
    """因子API —— Python与C++因子引擎的桥梁

    交互方式：
    1. pybind11 直接调用C++因子计算函数（推荐，低延迟）
    2. Arrow IPC 共享内存通信（大批量数据传输）
    3. HTTP/gRPC 远程调用（分布式场景）

    使用示例：
        api = FactorAPI(engine="cpp_binding")
        values = api.calculate("momentum_20d", ["000001.SZ", "600000.SH"], date(2025, 1, 10))
    """

    def __init__(
        self,
        engine: str = "cpp_binding",   # "cpp_binding" | "arrow_ipc" | "grpc"
        config: Optional[dict] = None,
    ):
        self._engine_type = engine
        self._config = config or {}
        self._engine: Optional[object] = None  # C++引擎实例
        self._initialize_engine()

    def calculate(
        self,
        factor_name: str,
        symbols: list[str],
        date: date,
        **kwargs,
    ) -> pd.Series:
        """计算单个因子

        Args:
            factor_name: 因子名（需在C++引擎中注册）
            symbols: 标的列表
            date: 计算日期
            **kwargs: 因子特有参数

        Returns:
            pd.Series(index=symbol, values=factor_value)
        """
        ...

    def calculate_batch(
        self,
        factor_names: list[str],
        symbols: list[str],
        start_date: date,
        end_date: date,
    ) -> pd.DataFrame:
        """批量计算多个因子

        Returns:
            DataFrame with MultiIndex (date, symbol) as index,
            columns = factor_names
        """
        ...

    def list_factors(self) -> list[dict]:
        """列出所有可用因子及其说明"""
        ...

    def _initialize_engine(self) -> None:
        """初始化C++因子引擎连接"""
        if self._engine_type == "cpp_binding":
            import quant_invest._cpp_bindings as _cpp
            self._engine = _cpp.FactorEngine(self._config)
        elif self._engine_type == "arrow_ipc":
            # Arrow IPC 通信初始化
            ...
        elif self._engine_type == "grpc":
            # gRPC 客户端初始化
            ...
```

```python
# strategy/optimizer.py

from dataclasses import dataclass
from datetime import date
from typing import Optional, Any, Callable
from itertools import product
import pandas as pd


@dataclass
class ParamRange:
    """参数搜索范围"""
    name: str
    low: float
    high: float
    step: Optional[float] = None
    values: Optional[list] = None  # 离散值列表


@dataclass
class OptimizationResult:
    """优化结果"""
    best_params: dict
    best_score: float
    all_results: pd.DataFrame    # 所有参数组合及对应评分
    optimization_metric: str


class StrategyOptimizer:
    """策略参数优化框架

    支持优化方法：
    1. 网格搜索（Grid Search）
    2. 随机搜索（Random Search）
    3. 贝叶斯优化（Optuna）
    4. 遗传算法（可选）

    防过拟合措施：
    1. 样本内/外分割
    2. Walk-forward 分析
    3. 多折交叉验证
    """

    def __init__(
        self,
        strategy_class: type,
        backtest_engine_class: type = None,
        metric: str = "sharpe_ratio",
        method: str = "grid",
    ):
        self.strategy_class = strategy_class
        self.backtest_engine_class = backtest_engine_class
        self.metric = metric
        self.method = method

    def optimize(
        self,
        param_ranges: list[ParamRange],
        start_date: date,
        end_date: date,
        symbols: list[str],
        method: Optional[str] = None,
        n_trials: int = 100,
        split_ratio: float = 0.7,
    ) -> OptimizationResult:
        """执行参数优化"""
        ...

    def walk_forward_analysis(
        self,
        param_ranges: list[ParamRange],
        train_period: int,     # 训练窗口（天）
        test_period: int,      # 测试窗口（天）
        start_date: date,
        end_date: date,
        symbols: list[str],
    ) -> list[OptimizationResult]:
        """Walk-forward分析"""
        ...
```

```python
# strategy/registry.py

from typing import Type, Dict, Optional


class StrategyRegistry:
    """策略注册中心

    提供策略的统一注册、发现和实例化：
    - 通过装饰器自动注册策略
    - 支持按名称查找策略类
    - 支持策略版本管理
    """

    _registry: Dict[str, Type[StrategyBase]] = {}

    @classmethod
    def register(cls, name: str):
        """策略注册装饰器"""
        def decorator(strategy_cls: Type[StrategyBase]):
            cls._registry[name] = strategy_cls
            strategy_cls._registry_name = name
            return strategy_cls
        return decorator

    @classmethod
    def get(cls, name: str) -> Type[StrategyBase]:
        """按名称查找策略"""
        if name not in cls._registry:
            raise KeyError(f"Strategy '{name}' not registered")
        return cls._registry[name]

    @classmethod
    def list_strategies(cls) -> list[str]:
        """列出所有已注册策略"""
        return list(cls._registry.keys())


# 使用示例：
# @StrategyRegistry.register("momentum_alpha")
# class MomentumAlphaStrategy(StrategyBase):
#     ...
```

### 5.2 数据流描述

```
[回测引擎] ──MarketEvent──> [Strategy.on_bar()]
                                 │
                          ┌──────┴──────┐
                          │             │
                     [上下文数据]    [因子API]
                          │             │
                          │        (pybind11)
                          │             │
                          │        [C++因子引擎]
                          │             │
                          ▼             ▼
                    [信号生成] ──> SignalEvent
                          │
                    [信号组合]
                   (多策略合并)
                          │
                          ▼
                    [组合管理器] ──> OrderEvent
                          │
                          ▼
                    [模拟经纪商] ──> FillEvent
```

### 5.3 模块间交互

- **策略 → 数据采集**：通过 `StrategyContext` 访问 `DataHandler`
- **策略 → C++因子引擎**：通过 `FactorAPI` 跨语言调用
- **策略 → 回测**：策略作为回测引擎的组成部分被驱动
- **策略 → ML管道**：ML模型可作为策略的信号源之一
- **策略 → API**：注册和实例化策略通过 FastAPI 管理

---

## 6. ML/AI管道

### 6.1 核心类/接口定义

```python
# ml/feature/builder.py

from abc import ABC, abstractmethod
from datetime import date
from dataclasses import dataclass
from typing import Optional
import pandas as pd


@dataclass
class FeatureConfig:
    """特征配置"""
    name: str
    source: str             # "cpp_factor" | "python_calc" | "external"
    transform: str = "raw"  # "raw" | "rank" | "zscore" | "demean" | "neutralize"
    lookback: int = 0       # 回看窗口
    params: dict = None


class FeatureBuilder:
    """特征构建器

    负责从多数据源构建ML特征矩阵：
    1. 调用C++因子引擎获取因子值
    2. 计算Python层衍生特征
    3. 执行特征变换（标准化/排名/中性化）
    4. 拼接标签（未来收益率）
    """

    def __init__(
        self,
        factor_api: Optional["FactorAPI"] = None,
        data_provider: Optional["DataProvider"] = None,
    ):
        self.factor_api = factor_api
        self.data_provider = data_provider
        self._feature_configs: list[FeatureConfig] = []

    def add_feature(self, config: FeatureConfig) -> "FeatureBuilder":
        """添加特征（链式调用）"""
        self._feature_configs.append(config)
        return self

    def add_factor_features(self, factor_names: list[str], transform: str = "rank") -> "FeatureBuilder":
        """批量添加因子特征"""
        for name in factor_names:
            self.add_feature(FeatureConfig(
                name=name, source="cpp_factor", transform=transform
            ))
        return self

    def build(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        label_lookforward: int = 5,
        label_type: str = "return",
    ) -> tuple[pd.DataFrame, pd.Series]:
        """构建特征矩阵和标签

        Returns:
            X: DataFrame(index=(date, symbol), columns=feature_names)
            y: Series(index=(date, symbol), name=label)
        """
        ...

    def _compute_cpp_factor(self, config: FeatureConfig, symbols: list[str],
                            start_date: date, end_date: date) -> pd.DataFrame:
        """从C++因子引擎获取特征"""
        ...

    def _compute_python_feature(self, config: FeatureConfig, symbols: list[str],
                                start_date: date, end_date: date) -> pd.DataFrame:
        """Python层计算衍生特征"""
        ...

    def _apply_transform(self, df: pd.DataFrame, transform: str) -> pd.DataFrame:
        """特征变换"""
        ...


class FeatureTransformer:
    """特征变换管道

    支持变换：
    - zscore: 横截面标准化
    - rank: 横截面排名
    - demean: 去均值
    - neutralize: 行业中性化（一元回归残差）
    - winsorize: 去极值
    - fillna: 缺失值填充
    """

    def zscore(self, df: pd.DataFrame, groupby: str = "date") -> pd.DataFrame:
        ...

    def rank(self, df: pd.DataFrame, groupby: str = "date") -> pd.DataFrame:
        ...

    def neutralize(
        self,
        df: pd.DataFrame,
        industry_map: pd.Series,
        groupby: str = "date",
    ) -> pd.DataFrame:
        """行业中性化"""
        ...

    def winsorize(
        self,
        df: pd.DataFrame,
        n_sigma: float = 3.0,
    ) -> pd.DataFrame:
        """去极值"""
        ...
```

```python
# ml/models/xgboost_model.py (以及 lstm_model.py, transformer_model.py)

from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime
from typing import Optional
import pandas as pd
import numpy as np


@dataclass
class ModelConfig:
    """模型配置基类"""
    model_type: str
    hyperparams: dict
    random_seed: int = 42


class ModelBase(ABC):
    """模型基类"""

    @abstractmethod
    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "ModelBase":
        """训练模型"""
        ...

    @abstractmethod
    def predict(self, X: pd.DataFrame) -> np.ndarray:
        """预测"""
        ...

    @abstractmethod
    def save(self, path: str) -> None:
        """保存模型"""
        ...

    @abstractmethod
    def load(self, path: str) -> None:
        """加载模型"""
        ...


class XGBoostModel(ModelBase):
    """XGBoost模型封装

    适用场景：因子选股、截面收益预测
    特点：表格数据、特征重要性可解释、训练快
    """

    def __init__(self, config: Optional[dict] = None):
        self.config = config or {
            "n_estimators": 500,
            "max_depth": 6,
            "learning_rate": 0.05,
            "subsample": 0.8,
            "colsample_bytree": 0.8,
            "reg_alpha": 0.1,
            "reg_lambda": 1.0,
        }
        self.model = None

    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "XGBoostModel":
        ...

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        ...

    def feature_importance(self) -> pd.Series:
        """获取特征重要性"""
        ...


class LSTMModel(ModelBase):
    """LSTM模型封装

    适用场景：时序预测、动量因子
    特点：捕捉序列模式、需GPU加速
    """

    def __init__(self, config: Optional[dict] = None):
        self.config = config or {
            "hidden_size": 64,
            "num_layers": 2,
            "dropout": 0.2,
            "learning_rate": 1e-3,
            "sequence_length": 20,
        }
        self.model = None

    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "LSTMModel":
        ...

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        ...


class TransformerModel(ModelBase):
    """Transformer模型封装

    适用场景：多因子时序预测、注意力机制选股
    特点：长程依赖、多头注意力、需GPU
    """

    def __init__(self, config: Optional[dict] = None):
        self.config = config or {
            "d_model": 64,
            "nhead": 4,
            "num_layers": 2,
            "dim_feedforward": 128,
            "dropout": 0.1,
        }
        self.model = None

    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "TransformerModel":
        ...

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        ...
```

```python
# ml/pipeline.py

from dataclasses import dataclass
from datetime import date
from typing import Optional
import pandas as pd


@dataclass
class TrainConfig:
    """训练配置"""
    train_start: date
    train_end: date
    valid_start: date
    valid_end: date
    test_start: date
    test_end: date
    retrain_interval: int = 20        # 重新训练间隔（交易日）
    walk_forward: bool = True          # 是否walk-forward


class MLPipeline:
    """ML训练管线

    完整的模型训练流程：
    1. 特征构建 → FeatureBuilder
    2. 数据分割 → 时间序列切分（避免未来信息泄露）
    3. 模型训练 → XGBoost/LSTM/Transformer
    4. 模型评估 → Evaluation
    5. 模型注册 → Versioning
    6. 回测集成 → 预测信号输入回测

    Walk-forward训练模式：
    ┌──── train ────┐┌─ valid ─┐
    │                ││         │          test
    │                ││         │┌────────┐
    │                ││         ││        │
    ──────────────────────────────────────────> time
    """

    def __init__(
        self,
        feature_builder: "FeatureBuilder",
        model: "ModelBase",
        evaluator: Optional["ModelEvaluator"] = None,
        versioning: Optional["ModelVersioning"] = None,
    ):
        self.feature_builder = feature_builder
        self.model = model
        self.evaluator = evaluator or ModelEvaluator()
        self.versioning = versioning or ModelVersioning()

    def train(
        self,
        symbols: list[str],
        config: TrainConfig,
    ) -> dict:
        """执行模型训练"""
        # 1. 构建特征
        X, y = self.feature_builder.build(
            symbols=symbols,
            start_date=config.train_start,
            end_date=config.train_end,
        )

        # 2. 时间序列切分
        X_train, X_valid, X_test = self._split_time_series(X, config)
        y_train, y_valid, y_test = self._split_time_series(y, config)

        # 3. 训练模型
        self.model.fit(X_train, y_train, eval_set=(X_valid, y_valid))

        # 4. 评估
        metrics = self.evaluator.evaluate(self.model, X_test, y_test)

        # 5. 注册模型版本
        version = self.versioning.register(self.model, metrics, config)

        return {"metrics": metrics, "version": version, "model": self.model}

    def train_walk_forward(
        self,
        symbols: list[str],
        config: TrainConfig,
    ) -> list[dict]:
        """Walk-forward训练"""
        ...

    def predict(
        self,
        symbols: list[str],
        date: date,
        version: Optional[str] = None,
    ) -> pd.Series:
        """使用指定版本的模型预测"""
        ...

    def _split_time_series(self, data, config: TrainConfig):
        """时间序列分割（严格按时间，杜绝未来信息泄露）"""
        ...
```

```python
# ml/evaluation.py

from dataclasses import dataclass
from typing import Optional
import pandas as pd
import numpy as np


@dataclass
class EvaluationMetrics:
    """模型评估指标"""
    # 回归指标
    mse: float
    rmse: float
    mae: float
    r2: float

    # 排名指标（因子预测更关注排名）
    rank_ic: float          # Rank IC
    rank_ic_ir: float       # IC信息比率
    top_decile_return: float  # 头部十分位收益率
    monotonicity: float     # 分组收益率单调性

    # 分类指标（如涨跌预测）
    accuracy: Optional[float] = None
    precision: Optional[float] = None
    recall: Optional[float] = None
    auc: Optional[float] = None


class ModelEvaluator:
    """模型评估器

    量化模型评估重点：
    1. IC/IR（比MSE更重要）
    2. 分层回测（分10组看单调性）
    3. 换手率和交易成本影响
    4. 回测集成验证
    """

    def evaluate(
        self,
        model: "ModelBase",
        X_test: pd.DataFrame,
        y_test: pd.Series,
    ) -> EvaluationMetrics:
        """综合评估模型"""
        predictions = model.predict(X_test)
        ...

    @staticmethod
    def calc_rank_ic(predictions: pd.Series, actual: pd.Series) -> float:
        """计算排名IC"""
        ...

    @staticmethod
    def calc_layer_backtest(
        predictions: pd.Series,
        actual: pd.Series,
        n_layers: int = 10,
    ) -> pd.DataFrame:
        """分层回测"""
        ...

    def evaluate_in_backtest(
        self,
        model: "ModelBase",
        symbols: list[str],
        start_date: "date",
        end_date: "date",
        backtest_engine: "BacktestEngine",
    ) -> "PerformanceMetrics":
        """将模型预测集成到回测中验证"""
        ...
```

```python
# ml/versioning.py

from dataclasses import dataclass, field
from datetime import datetime
from typing import Optional
import json
import hashlib


@dataclass
class ModelVersion:
    """模型版本"""
    version_id: str
    model_type: str
    created_at: datetime
    metrics: dict
    config: dict
    feature_list: list[str]
    training_data_hash: str
    file_path: str
    is_production: bool = False


class ModelVersioning:
    """模型版本管理

    功能：
    1. 模型注册与版本号分配
    2. 生产/实验标记
    3. 模型回滚
    4. 模型元数据存储
    5. A/B测试支持
    """

    def __init__(self, storage_path: str = "./model_registry"):
        self.storage_path = storage_path
        self._versions: dict[str, ModelVersion] = {}

    def register(
        self,
        model: "ModelBase",
        metrics: dict,
        config: dict,
        feature_list: list[str],
    ) -> ModelVersion:
        """注册新模型版本"""
        ...

    def get_production(self, model_type: str) -> Optional[ModelVersion]:
        """获取当前生产版本"""
        ...

    def promote(self, version_id: str) -> None:
        """将版本提升为生产版本"""
        ...

    def rollback(self, model_type: str, to_version: str) -> None:
        """回滚到指定版本"""
        ...

    def list_versions(self, model_type: str = "") -> list[ModelVersion]:
        """列出所有版本"""
        ...
```

### 6.2 数据流描述

```
[数据源] ──> [FeatureBuilder] ──> 特征矩阵(X) + 标签(y)
                    │
         ┌──────────┴──────────┐
         │                      │
    [C++因子引擎]         [Python衍生特征]
    (pybind11调用)        (技术指标/统计量)
         │                      │
         └──────────┬───────────┘
                    │
             [FeatureTransformer]
             (标准化/中性化/去极值)
                    │
                    ▼
            [数据分割(时间序列)]
                    │
         ┌──────────┼──────────┐
         ▼          ▼          ▼
      [train]    [valid]    [test]
         │
    [Model.fit()]
         │
    ┌────┴────┐
    │          │
  [XGBoost] [LSTM/Transformer]
    │          │
    └────┬─────┘
         │
    [Evaluation]
    (IC/IR/分层回测)
         │
    [Versioning] ──> 注册模型版本
         │
    [回测集成] ──> MLStrategy 在回测框架中验证
```

### 6.3 模块间交互

- **ML → 数据采集**：`FeatureBuilder` 通过 `DataProvider` 获取原始数据
- **ML → C++因子引擎**：通过 `FactorAPI` 获取因子作为ML特征
- **ML → 回测**：通过 `MLStrategy` 将模型预测集成到回测框架
- **ML → API**：模型管理、训练任务通过 FastAPI 暴露

---

## 7. API服务层

### 7.1 核心类/接口定义

```python
# api/app.py

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from contextlib import asynccontextmanager

from .routes import strategy, backtest, portfolio, factor, system
from .ws import WebSocketManager
from .auth import AuthMiddleware
from ..config.settings import Settings


@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用生命周期管理"""
    # Startup
    settings = Settings()
    app.state.ws_manager = WebSocketManager()
    app.state.settings = settings
    # 初始化各模块...
    yield
    # Shutdown
    await app.state.ws_manager.close()


def create_app() -> FastAPI:
    """创建FastAPI应用实例"""
    app = FastAPI(
        title="QuantInvest API",
        version="0.1.0",
        description="A股量化投资系统API",
        lifespan=lifespan,
    )

    # 中间件
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],  # 生产环境应限制
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.add_middleware(AuthMiddleware)

    # 路由注册
    app.include_router(strategy.router, prefix="/api/v1/strategy", tags=["策略管理"])
    app.include_router(backtest.router, prefix="/api/v1/backtest", tags=["回测"])
    app.include_router(portfolio.router, prefix="/api/v1/portfolio", tags=["持仓组合"])
    app.include_router(factor.router, prefix="/api/v1/factor", tags=["因子"])
    app.include_router(system.router, prefix="/api/v1/system", tags=["系统"])

    return app


app = create_app()
```

```python
# api/schemas.py

from datetime import date, datetime
from typing import Optional, Any
from pydantic import BaseModel, Field


# ── 策略相关 ──

class StrategyCreate(BaseModel):
    """创建策略请求"""
    name: str = Field(..., description="策略名称")
    type: str = Field(..., description="策略类型")
    params: dict = Field(default_factory=dict, description="策略参数")
    description: str = Field("", description="策略描述")


class StrategyResponse(BaseModel):
    """策略信息响应"""
    id: str
    name: str
    type: str
    params: dict
    status: str  # "draft" | "running" | "paused" | "stopped"
    created_at: datetime
    updated_at: datetime


# ── 回测相关 ──

class BacktestRequest(BaseModel):
    """回测请求"""
    strategy_id: str = Field(..., description="策略ID")
    symbols: list[str] = Field(..., description="标的列表")
    start_date: date = Field(..., description="开始日期")
    end_date: date = Field(..., description="结束日期")
    frequency: str = Field("daily", description="数据频率")
    initial_cash: float = Field(1_000_000, description="初始资金")
    commission_config: Optional[dict] = None
    restriction_config: Optional[dict] = None


class BacktestResultResponse(BaseModel):
    """回测结果响应"""
    backtest_id: str
    status: str  # "pending" | "running" | "completed" | "failed"
    metrics: Optional[dict] = None
    nav_curve: Optional[list[dict]] = None  # [{date, nav}]
    trades: Optional[list[dict]] = None


# ── 持仓相关 ──

class PortfolioSnapshot(BaseModel):
    """组合快照"""
    total_value: float
    cash: float
    positions: list[dict]
    updated_at: datetime


# ── 因子相关 ──

class FactorCalcRequest(BaseModel):
    """因子计算请求"""
    factor_names: list[str]
    symbols: list[str]
    date: date


class FactorCalcResponse(BaseModel):
    """因子计算响应"""
    factors: dict[str, dict[str, float]]  # {factor_name: {symbol: value}}
```

```python
# api/routes/backtest.py

from fastapi import APIRouter, BackgroundTasks, HTTPException
from ..schemas import BacktestRequest, BacktestResultResponse
from ..ws import WebSocketManager

router = APIRouter()


@router.post("/run", response_model=dict)
async def run_backtest(
    request: BacktestRequest,
    background_tasks: BackgroundTasks,
):
    """提交回测任务（异步执行）"""
    # 1. 创建回测任务
    backtest_id = create_backtest_task(request)
    # 2. 后台执行回测
    background_tasks.add_task(execute_backtest, backtest_id, request)
    return {"backtest_id": backtest_id, "status": "pending"}


@router.get("/result/{backtest_id}", response_model=BacktestResultResponse)
async def get_backtest_result(backtest_id: str):
    """查询回测结果"""
    ...


@router.get("/list", response_model=list[BacktestResultResponse])
async def list_backtests(
    strategy_id: str = "",
    status: str = "",
    limit: int = 20,
):
    """列出回测任务"""
    ...


@router.websocket("/ws/{backtest_id}")
async def backtest_progress_ws(websocket, backtest_id: str):
    """WebSocket推送回测进度"""
    ...
```

```python
# api/auth.py

from fastapi import Request, HTTPException
from starlette.middleware.base import BaseHTTPMiddleware
from typing import Optional
import jwt


class AuthMiddleware(BaseHTTPMiddleware):
    """认证中间件

    支持两种认证方式：
    1. JWT Token（API调用）
    2. API Key（机器人调用）

    白名单路径无需认证：/api/v1/system/health, /docs, /openapi.json
    """

    WHITELIST = {"/api/v1/system/health", "/docs", "/openapi.json", "/redoc"}

    async def dispatch(self, request: Request, call_next):
        if request.url.path in self.WHITELIST:
            return await call_next(request)

        token = request.headers.get("Authorization", "")
        api_key = request.headers.get("X-API-Key", "")

        if token:
            # JWT验证
            try:
                payload = jwt.decode(token, self._secret, algorithms=["HS256"])
                request.state.user = payload
            except jwt.InvalidTokenError:
                raise HTTPException(status_code=401, detail="Invalid token")
        elif api_key:
            # API Key验证
            user = self._validate_api_key(api_key)
            if not user:
                raise HTTPException(status_code=401, detail="Invalid API key")
            request.state.user = user
        else:
            raise HTTPException(status_code=401, detail="Authentication required")

        return await call_next(request)
```

```python
# api/ws.py

from fastapi import WebSocket
from typing import Dict, Set
import asyncio
import json


class WebSocketManager:
    """WebSocket连接管理器

    用途：
    1. 回测进度实时推送
    2. 实时持仓更新
    3. 交易信号推送
    4. 系统状态通知
    """

    def __init__(self):
        self._connections: Dict[str, Set[WebSocket]] = {}

    async def connect(self, channel: str, websocket: WebSocket):
        """建立WebSocket连接"""
        await websocket.accept()
        if channel not in self._connections:
            self._connections[channel] = set()
        self._connections[channel].add(websocket)

    async def disconnect(self, channel: str, websocket: WebSocket):
        """断开连接"""
        self._connections[channel].discard(websocket)

    async def broadcast(self, channel: str, message: dict):
        """向频道内所有连接广播消息"""
        if channel not in self._connections:
            return
        disconnected = set()
        for ws in self._connections[channel]:
            try:
                await ws.send_json(message)
            except Exception:
                disconnected.add(ws)
        self._connections[channel] -= disconnected

    async def close(self):
        """关闭所有连接"""
        for channel in self._connections:
            for ws in self._connections[channel]:
                await ws.close()
```

### 7.2 API接口清单

| 类别 | 方法 | 路径 | 说明 |
|------|------|------|------|
| 策略管理 | POST | `/api/v1/strategy/create` | 创建策略 |
| | GET | `/api/v1/strategy/list` | 策略列表 |
| | GET | `/api/v1/strategy/{id}` | 策略详情 |
| | PUT | `/api/v1/strategy/{id}/params` | 更新参数 |
| | POST | `/api/v1/strategy/{id}/start` | 启动策略 |
| | POST | `/api/v1/strategy/{id}/stop` | 停止策略 |
| 回测 | POST | `/api/v1/backtest/run` | 提交回测 |
| | GET | `/api/v1/backtest/result/{id}` | 回测结果 |
| | GET | `/api/v1/backtest/list` | 回测列表 |
| | WS | `/api/v1/backtest/ws/{id}` | 回测进度推送 |
| 持仓 | GET | `/api/v1/portfolio/snapshot` | 当前持仓快照 |
| | GET | `/api/v1/portfolio/history` | 持仓历史 |
| | GET | `/api/v1/portfolio/performance` | 绩效分析 |
| 因子 | POST | `/api/v1/factor/calculate` | 因子计算 |
| | GET | `/api/v1/factor/list` | 因子列表 |
| | GET | `/api/v1/factor/{name}/info` | 因子详情 |
| 系统 | GET | `/api/v1/system/health` | 健康检查 |
| | GET | `/api/v1/system/status` | 系统状态 |

### 7.3 模块间交互

- **API → 回测**：创建回测任务、查询结果、WebSocket推送进度
- **API → 策略**：CRUD策略、启停控制
- **API → 持仓**：查询当前组合和历史
- **API → 因子**：触发因子计算、查询因子信息
- **API → ML**：触发模型训练、查询模型版本
- **API → 飞书机器人**：部分接口被飞书Webhook调用

---

## 8. 飞书机器人交互

### 8.1 核心类/接口定义

```python
# bot/app.py

from fastapi import FastAPI, Request
from typing import Optional
import hashlib
import hmac
import json

from .commands import portfolio, performance, strategy_ctrl
from .pusher import SignalPusher
from .templates import MessageTemplates


class FeishuBot:
    """飞书机器人核心

    处理流程：
    1. 接收飞书事件推送（POST /feishu/webhook）
    2. 验证签名
    3. 解析事件类型
    4. 路由到对应处理器
    5. 返回响应
    """

    def __init__(
        self,
        app_id: str,
        app_secret: str,
        verification_token: str,
        encrypt_key: Optional[str] = None,
    ):
        self.app_id = app_id
        self.app_secret = app_secret
        self.verification_token = verification_token
        self.encrypt_key = encrypt_key
        self.command_handlers: dict[str, "CommandHandler"] = {}
        self.pusher = SignalPusher()
        self.templates = MessageTemplates()
        self._register_commands()

    def _register_commands(self):
        """注册指令处理器"""
        self.command_handlers = {
            "持仓": portfolio.PortfolioCommand(),
            "收益": performance.PerformanceCommand(),
            "策略": strategy_ctrl.StrategyCommand(),
            "回测": performance.BacktestCommand(),
            "因子": performance.FactorCommand(),
            "帮助": HelpCommand(),
        }

    async def handle_event(self, event: dict) -> dict:
        """处理飞书事件"""
        msg_type = event.get("msg_type")
        if msg_type == "text":
            text = event["event"]["message"]["content"].strip()
            return await self._dispatch_command(text, event)
        elif msg_type == "interactive":
            # 卡片交互回调
            action = event.get("action", {})
            return await self._handle_card_action(action)
        return {"success": False}

    async def _dispatch_command(self, text: str, event: dict) -> dict:
        """指令路由"""
        # 解析指令关键字
        for keyword, handler in self.command_handlers.items():
            if text.startswith(keyword) or text.startswith(f"/{keyword}"):
                return await handler.handle(text, event)
        return await self.command_handlers["帮助"].handle(text, event)

    def verify_signature(self, body: bytes, signature: str) -> bool:
        """验证飞书事件签名"""
        ...
```

```python
# bot/commands/portfolio.py

from abc import ABC, abstractmethod
from typing import Optional
from ..templates import MessageTemplates


class CommandHandler(ABC):
    """指令处理器基类"""
    @abstractmethod
    async def handle(self, text: str, event: dict) -> dict:
        ...


class PortfolioCommand(CommandHandler):
    """持仓查询指令

    指令格式：
    /持仓              → 查看全部持仓
    /持仓 000001.SZ    → 查看单只股票持仓
    /持仓 汇总          → 持仓汇总卡片
    """

    async def handle(self, text: str, event: dict) -> dict:
        args = text.split()[1:] if len(text.split()) > 1 else []
        if not args:
            # 返回全部持仓
            positions = self._get_all_positions()
            return MessageTemplates.format_portfolio_card(positions)
        elif args[0] == "汇总":
            summary = self._get_portfolio_summary()
            return MessageTemplates.format_portfolio_summary_card(summary)
        else:
            # 查询单只股票
            symbol = args[0]
            position = self._get_position(symbol)
            return MessageTemplates.format_single_position_card(position)

    def _get_all_positions(self) -> list[dict]:
        """从API服务获取持仓数据"""
        ...

    def _get_position(self, symbol: str) -> dict:
        ...

    def _get_portfolio_summary(self) -> dict:
        ...
```

```python
# bot/pusher.py

from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from typing import Optional
import httpx


class PushType(str, Enum):
    TRADE_SIGNAL = "trade_signal"       # 交易信号
    RISK_ALERT = "risk_alert"           # 风控告警
    DAILY_REPORT = "daily_report"       # 日报
    BACKTEST_DONE = "backtest_done"     # 回测完成
    MODEL_TRAIN_DONE = "model_done"     # 模型训练完成


@dataclass
class PushMessage:
    """推送消息"""
    type: PushType
    title: str
    content: dict
    timestamp: datetime = None
    priority: str = "normal"  # "low" / "normal" / "high" / "urgent"


class SignalPusher:
    """信号推送服务

    主动推送类型：
    1. 交易信号：策略产生的买卖信号
    2. 风控告警：回撤超限、仓位异常等
    3. 日报：每日收益、持仓变化等
    4. 任务通知：回测完成、模型训练完成等

    推送方式：
    - 飞书群卡片消息
    - 飞书个人消息
    - 飞书Webhook（简单通知）
    """

    def __init__(self, webhook_url: str = ""):
        self.webhook_url = webhook_url
        self.templates = MessageTemplates()

    async def push_trade_signal(self, signal: dict) -> dict:
        """推送交易信号"""
        msg = PushMessage(
            type=PushType.TRADE_SIGNAL,
            title="交易信号",
            content=signal,
            priority="high",
        )
        return await self._send(msg)

    async def push_risk_alert(self, alert: dict) -> dict:
        """推送风控告警"""
        msg = PushMessage(
            type=PushType.RISK_ALERT,
            title="风控告警",
            content=alert,
            priority="urgent",
        )
        return await self._send(msg)

    async def push_daily_report(self, report: dict) -> dict:
        """推送日报"""
        msg = PushMessage(
            type=PushType.DAILY_REPORT,
            title="日报",
            content=report,
            priority="normal",
        )
        return await self._send(msg)

    async def _send(self, message: PushMessage) -> dict:
        """发送消息到飞书"""
        card = self.templates.build_push_card(message)
        async with httpx.AsyncClient() as client:
            response = await client.post(
                self.webhook_url,
                json=card,
            )
            return response.json()
```

```python
# bot/templates.py

from typing import Optional


class MessageTemplates:
    """飞书消息模板管理

    飞书卡片消息结构：
    https://open.feishu.cn/document/common/cards
    """

    @staticmethod
    def format_portfolio_card(positions: list[dict]) -> dict:
        """持仓卡片消息"""
        elements = []
        for pos in positions:
            pnl_color = "green" if pos.get("pnl_pct", 0) >= 0 else "red"
            elements.append({
                "tag": "column_set",
                "columns": [
                    {"tag": "col", "width": "auto", "elements": [
                        {"tag": "markdown", "content": f"**{pos['symbol']}** {pos['name']}"}
                    ]},
                    {"tag": "col", "width": "auto", "elements": [
                        {"tag": "markdown", "content": f"<font color='{pnl_color}'>{pos['pnl_pct']:+.2f}%</font>"}
                    ]},
                ]
            })
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📊 当前持仓"}},
                "elements": elements,
            }
        }

    @staticmethod
    def format_portfolio_summary_card(summary: dict) -> dict:
        """持仓汇总卡片"""
        ...

    @staticmethod
    def format_performance_card(metrics: dict) -> dict:
        """绩效指标卡片"""
        ...

    @staticmethod
    def format_trade_signal_card(signal: dict) -> dict:
        """交易信号卡片"""
        ...

    @staticmethod
    def format_risk_alert_card(alert: dict) -> dict:
        """风控告警卡片"""
        ...

    @staticmethod
    def format_daily_report_card(report: dict) -> dict:
        """日报卡片"""
        ...

    @staticmethod
    def build_push_card(message: "PushMessage") -> dict:
        """根据推送消息类型选择模板并构建卡片"""
        template_map = {
            "trade_signal": MessageTemplates.format_trade_signal_card,
            "risk_alert": MessageTemplates.format_risk_alert_card,
            "daily_report": MessageTemplates.format_daily_report_card,
        }
        formatter = template_map.get(message.type.value)
        if formatter:
            return formatter(message.content)
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": message.title}},
                "elements": [
                    {"tag": "markdown", "content": str(message.content)}
                ],
            }
        }
```

### 8.2 数据流描述

```
飞书服务器
    │
    │ (1) 事件推送 (POST /feishu/webhook)
    ▼
[FeishuBot.handle_event()]
    │
    ├── 签名验证
    ├── 事件类型解析
    │
    ├── 文本消息 → 指令路由
    │       │
    │       ├── /持仓 → PortfolioCommand → [API: GET /portfolio/snapshot]
    │       ├── /收益 → PerformanceCommand → [API: GET /portfolio/performance]
    │       ├── /策略 → StrategyCommand → [API: POST /strategy/{id}/start|stop]
    │       ├── /回测 → BacktestCommand → [API: POST /backtest/run]
    │       └── /帮助 → HelpCommand
    │
    └── 卡片交互 → ActionHandler
            │
            └── 按钮回调处理


系统内部 ──> [SignalPusher] ──> 飞书Webhook
    │
    ├── 交易信号 ──> format_trade_signal_card()
    ├── 风控告警 ──> format_risk_alert_card()
    ├── 日报     ──> format_daily_report_card()
    └── 任务通知 ──> 通用卡片
```

### 8.3 模块间交互

- **飞书机器人 → API服务**：通过HTTP调用API获取数据
- **飞书机器人 → 回测**：触发回测、查询结果
- **飞书机器人 → 策略**：启停策略
- **API → 飞书机器人**：通过 `SignalPusher` 推送事件通知

---

## 9. 项目依赖管理

### 9.1 pyproject.toml

```toml
[project]
name = "quant-invest"
version = "0.1.0"
description = "A股量化投资系统 - Python策略研究层"
readme = "README.md"
license = { text = "MIT" }
requires-python = ">=3.11"
authors = [
    { name = "QuantInvest Team" },
]

dependencies = [
    # ── 数据处理 ──
    "pandas>=2.2",
    "numpy>=1.26",
    "polars>=0.20",              # 高性能DataFrame（可选）
    "pyarrow>=15.0",             # Arrow IPC / Parquet

    # ── 数据源 ──
    "akshare>=1.12",              # A股免费数据源
    "tushare>=1.4",               # Tushare Pro
    # wind: 需要Wind终端，通过whl本地安装

    # ── 回测 & 策略 ──
    # (本项目自研，无外部依赖)

    # ── ML/AI ──
    "scikit-learn>=1.4",
    "xgboost>=2.0",
    "lightgbm>=4.1",
    "torch>=2.2",                 # PyTorch (LSTM/Transformer)
    "optuna>=3.5",                # 超参优化

    # ── API ──
    "fastapi>=0.110",
    "uvicorn>=0.27",
    "websockets>=12.0",
    "httpx>=0.27",
    "python-jose[cryptography]>=3.3",  # JWT
    "python-multipart>=0.0.9",

    # ── 飞书 ──
    "feishu-sdk>=1.0",            # 飞书开放平台SDK

    # ── 配置 & 日志 ──
    "pydantic>=2.5",
    "pydantic-settings>=2.1",
    "loguru>=0.7",
    "python-dotenv>=1.0",

    # ── 调度 & 并发 ──
    "apscheduler>=3.10",
    "asyncio",
]

[project.optional-dependencies]
dev = [
    # ── 代码质量 ──
    "ruff>=0.3",                  # linter + formatter
    "mypy>=1.8",                  # 类型检查
    "pre-commit>=3.6",

    # ── 测试 ──
    "pytest>=8.0",
    "pytest-asyncio>=0.23",
    "pytest-cov>=4.1",
    "hypothesis>=6.98",            # 属性测试

    # ── Jupyter ──
    "jupyter>=1.0",
    "ipython>=8.20",
    "matplotlib>=3.8",
    "seaborn>=0.13",
]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["src/quant_invest"]

# ── Ruff 配置 ──
[tool.ruff]
target-version = "py311"
line-length = 100
src = ["src", "tests"]

[tool.ruff.lint]
select = [
    "E",    # pycodestyle errors
    "W",    # pycodestyle warnings
    "F",    # pyflakes
    "I",    # isort
    "N",    # pep8-naming
    "UP",   # pyupgrade
    "B",    # flake8-bugbear
    "SIM",  # flake8-simplify
    "TCH",  # flake8-type-checking
]
ignore = ["E501"]

[tool.ruff.lint.isort]
known-first-party = ["quant_invest"]

# ── mypy 配置 ──
[tool.mypy]
python_version = "3.11"
warn_return_any = true
warn_unused_configs = true
disallow_untyped_defs = true
check_untyped_defs = true

[[tool.mypy.overrides]]
module = ["akshare.*", "tushare.*", "torch.*"]
ignore_missing_imports = true

# ── pytest 配置 ──
[tool.pytest.ini_options]
testpaths = ["tests"]
asyncio_mode = "auto"
addopts = "-v --tb=short"
markers = [
    "slow: marks tests as slow (deselect with '-m \"not slow\"')",
    "integration: marks integration tests",
    "network: marks tests that require network access",
]

# ── Coverage 配置 ──
[tool.coverage.run]
source = ["quant_invest"]
omit = ["tests/*"]

[tool.coverage.report]
fail_under = 80
show_missing = true
```

### 9.2 开发环境搭建

**推荐使用 uv（快速、安全）：**

```bash
# 安装 uv
curl -LsSf https://astral.sh/uv/install.sh | sh

# 创建虚拟环境并安装依赖
cd quant_invest/py
uv venv --python 3.11
source .venv/bin/activate
uv pip install -e ".[dev]"

# 或使用 poetry（备选）
# poetry install --with dev
```

### 9.3 代码质量工具链

```bash
# 格式化 + Lint（Ruff 统一处理）
ruff format .              # 代码格式化
ruff check . --fix         # Lint检查 + 自动修复

# 类型检查
mypy src/quant_invest

# 测试
pytest                     # 全量测试
pytest -m "not slow"       # 跳过慢测试
pytest --cov=quant_invest # 覆盖率报告

# Pre-commit 钩子
pre-commit install
# .pre-commit-config.yaml 中配置 ruff, mypy
```

---

## 10. 模块间交互总览

### 10.1 运行时交互矩阵

```
                数据采集  回测框架  策略研究  ML管道   API服务   飞书机器人
  数据采集         ─       提供      提供     提供     查询       ─
  回测框架       请求       ─      驱动      ─      暴露       ─
  策略研究       请求     被驱动      ─     信号源   管理      控制
  ML管道        请求      回测     因子源      ─     管理       ─
  API服务        调用     触发      CRUD    触发      ─      被调用
  飞书机器人      ─      触发     控制      ─     调用        ─
```

### 10.2 数据流向图

```
                          ┌─────────────┐
                          │  外部数据源   │
                          │(akshare/    │
                          │ tushare/wind)│
                          └──────┬──────┘
                                 │
                                 ▼
                          ┌─────────────┐
                          │  数据采集模块  │
                          │ (providers/  │
                          │  collectors) │
                          └──────┬──────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
                    ▼            ▼            ▼
             ┌──────────┐ ┌──────────┐ ┌──────────┐
             │ 回测框架  │ │ 策略研究  │ │  ML管道   │
             └─────┬────┘ └─────┬────┘ └─────┬────┘
                   │            │            │
                   │      ┌─────┴─────┐      │
                   │      │ FactorAPI  │      │
                   │      │(pybind11)  │      │
                   │      └─────┬─────┘      │
                   │            │            │
                   └────────────┼────────────┘
                                │
                     ┌──────────┴──────────┐
                     │   C++/Rust 因子引擎   │
                     └─────────────────────┘
                                │
                   ┌────────────┼────────────┐
                   │            │            │
                   ▼            ▼            ▼
             ┌──────────┐ ┌──────────┐ ┌──────────┐
             │ API服务   │ │ 键值存储  │ │  时序DB   │
             │ (FastAPI) │ │(元数据)   │ │(行情数据)  │
             └─────┬────┘ └──────────┘ └──────────┘
                   │
                   ▼
             ┌──────────┐
             │ 飞书机器人  │
             └──────────┘
```

---

## 11. 与C++层交互接口

### 11.1 交互方式对比

| 方式 | 延迟 | 吞吐量 | 适用场景 | 实现复杂度 |
|------|------|--------|----------|------------|
| pybind11 | ~1μs | 高 | 因子计算、实时计算 | 中 |
| cFFI | ~1μs | 高 | 简单函数调用 | 低 |
| Arrow IPC | ~100μs | 极高 | 批量数据传输 | 中 |
| 共享内存 | ~10μs | 极高 | 高频tick数据 | 高 |
| gRPC | ~1ms | 中 | 分布式场景 | 高 |

**推荐方案：pybind11（主）+ Arrow IPC（辅）**

### 11.2 pybind11 接口定义

```python
# quant_invest/_cpp_bindings/__init__.pyi (类型存根)

from datetime import date
from typing import List
import pandas as pd


class FactorEngine:
    """C++因子计算引擎的Python绑定"""

    def __init__(self, config: dict = None) -> None: ...

    def calculate(
        self,
        factor_name: str,
        symbols: List[str],
        date: date,
        **kwargs,
    ) -> pd.Series: ...

    def calculate_batch(
        self,
        factor_names: List[str],
        symbols: List[str],
        start_date: date,
        end_date: date,
    ) -> pd.DataFrame: ...

    def list_factors(self) -> List[dict]: ...


class ExecutionEngine:
    """C++执行引擎的Python绑定"""

    def __init__(self, config: dict = None) -> None: ...

    def submit_order(
        self,
        symbol: str,
        direction: str,
        quantity: int,
        price: float = 0.0,
        order_type: str = "LIMIT",
    ) -> str: ...

    def cancel_order(self, order_id: str) -> bool: ...

    def get_order_status(self, order_id: str) -> dict: ...


# C++端需要暴露的函数（Rust通过PyO3暴露同理）
def calc_momentum(
    prices: pd.DataFrame,  # columns=symbols, index=dates
    window: int = 20,
) -> pd.DataFrame: ...


def calc_volatility(
    prices: pd.DataFrame,
    window: int = 20,
) -> pd.DataFrame: ...
```

### 11.3 Arrow IPC 数据交换

```python
# quant_invest/_cpp_bindings/arrow_transport.py

import pyarrow as pa
import pyarrow.ipc as ipc
from datetime import date
from typing import Optional


class ArrowTransport:
    """Arrow IPC 数据传输层

    用于大批量数据在Python和C++之间的零拷贝传输：
    1. Python构建Arrow RecordBatch
    2. 通过共享内存或socket传输
    3. C++端直接读取，无需序列化/反序列化

    典型场景：
    - 因子计算：Python传行情数据 → C++计算因子 → 返回因子值
    - 回测数据：Python加载历史数据 → C++高性能回测
    """

    def __init__(self, config: Optional[dict] = None):
        self.config = config or {}

    def send_to_cpp(
        self,
        data: pa.Table,
        channel: str = "factor_input",
    ) -> None:
        """发送Arrow Table到C++进程"""
        sink = pa.BufferOutputStream()
        writer = ipc.new_stream(sink, data.schema)
        writer.write_table(data)
        writer.close()
        # 通过共享内存/socket发送sink.getvalue()
        ...

    def receive_from_cpp(
        self,
        channel: str = "factor_output",
    ) -> pa.Table:
        """从C++进程接收Arrow Table"""
        # 从共享内存/socket读取
        ...
        reader = ipc.open_stream(buffer)
        return reader.read_all()

    @staticmethod
    def pandas_to_arrow(df: pd.DataFrame) -> pa.Table:
        """pandas DataFrame → Arrow Table"""
        return pa.Table.from_pandas(df)

    @staticmethod
    def arrow_to_pandas(table: pa.Table) -> pd.DataFrame:
        """Arrow Table → pandas DataFrame"""
        return table.to_pandas()
```

### 11.4 C++侧接口约定（供C++开发参考）

```cpp
// C++侧需要实现的接口（示意）
// 因子引擎必须实现以下方法以供 pybind11 绑定

class FactorEngine {
public:
    // 计算单个因子
    // 输入：因子名、标的列表、日期、参数
    // 输出：map<string, double> (symbol -> factor_value)
    std::unordered_map<std::string, double> calculate(
        const std::string& factor_name,
        const std::vector<std::string>& symbols,
        const std::string& date,
        const std::unordered_map<std::string, double>& params = {}
    );

    // 批量计算因子（高性能路径）
    // 输入/输出通过 Arrow IPC 共享内存
    void calculate_batch_arrow(
        ArrowSchema* input_schema,
        ArrowArray* input_array,
        ArrowSchema* output_schema,
        ArrowArray* output_array
    );

    // 列出所有可用因子
    std::vector<FactorInfo> list_factors();
};
```

---

## 附录A：关键配置示例

```python
# config/settings.py

from pydantic_settings import BaseSettings
from typing import Optional


class Settings(BaseSettings):
    """全局配置

    支持环境变量覆盖，优先级：
    环境变量 > .env文件 > 默认值
    """

    # ── 数据源 ──
    DATA_PROVIDER: str = "akshare"            # 默认数据源
    TUSHARE_TOKEN: Optional[str] = None        # Tushare Pro Token
    WIND_USERNAME: Optional[str] = None        # Wind账号
    WIND_PASSWORD: Optional[str] = None

    # ── 存储 ──
    DATA_STORAGE_PATH: str = "./data"          # 数据存储路径
    CACHE_ENABLED: bool = True                 # 是否启用本地缓存
    CACHE_PATH: str = "./cache"               # 缓存路径

    # ── C++引擎 ──
    CPP_ENGINE_MODE: str = "pybind11"          # "pybind11" | "arrow_ipc" | "grpc"
    CPP_ENGINE_CONFIG: dict = {}               # C++引擎配置

    # ── API ──
    API_HOST: str = "0.0.0.0"
    API_PORT: int = 8000
    API_WORKERS: int = 4
    JWT_SECRET: str = "change-me-in-production"
    JWT_EXPIRE_HOURS: int = 24

    # ── 飞书 ──
    FEISHU_APP_ID: str = ""
    FEISHU_APP_SECRET: str = ""
    FEISHU_VERIFICATION_TOKEN: str = ""
    FEISHU_WEBHOOK_URL: str = ""

    # ── 回测 ──
    BACKTEST_INITIAL_CASH: float = 1_000_000.0
    BACKTEST_COMMISSION_RATE: float = 0.00025
    BACKTEST_STAMP_TAX_RATE: float = 0.001

    # ── 日志 ──
    LOG_LEVEL: str = "INFO"
    LOG_PATH: str = "./logs"

    class Config:
        env_file = ".env"
        env_prefix = "QI_"  # 环境变量前缀: QI_DATA_PROVIDER=akshare
```

## 附录B：快速启动示例

```python
# scripts/run_backtest.py — 快速回测示例

from datetime import date
from quant_invest.data.providers.akshare_provider import AkshareProvider
from quant_invest.backtest.engine import BacktestEngine
from quant_invest.backtest.broker import SimulatedBroker, CommissionConfig
from quant_invest.backtest.data_handler import DailyDataHandler
from quant_invest.backtest.portfolio import Portfolio
from quant_invest.strategy.base import StrategyBase, StrategyParams
from quant_invest.strategy.registry import StrategyRegistry
from quant_invest.backtest.performance import PerformanceAnalyzer


# 1. 定义策略
@StrategyRegistry.register("simple_momentum")
class SimpleMomentum(StrategyBase):
    """简单动量策略示例"""

    class Params(StrategyParams):
        lookback: int = 20
        top_n: int = 10

    def on_init(self):
        self.params = self.Params()

    def on_bar(self, bar_data, positions):
        signals = []
        # 计算动量（通过上下文访问历史数据）
        for symbol, data in bar_data.items():
            hist = self.context.history(symbol, self.params.lookback)
            if hist is not None and len(hist) >= self.params.lookback:
                momentum = hist["close"].pct_change(self.params.lookback).iloc[-1]
                if momentum > 0.05:  # 5%动量阈值
                    signals.append(SignalEvent(
                        symbol=symbol,
                        direction="LONG",
                        strength=min(momentum, 1.0),
                    ))
        return signals


# 2. 配置回测
provider = AkshareProvider()
data_handler = DailyDataHandler(
    symbols=["000001.SZ", "600000.SH", "000858.SZ"],
    start_date=date(2024, 1, 1),
    end_date=date(2024, 12, 31),
    data_provider=provider,
)
broker = SimulatedBroker(CommissionConfig())
portfolio = Portfolio()
strategy = SimpleMomentum()

engine = BacktestEngine(
    data_handler=data_handler,
    broker=broker,
    portfolio=portfolio,
    strategy=strategy,
    initial_cash=1_000_000,
)

# 3. 运行回测
result = engine.run(
    start_date=date(2024, 1, 1),
    end_date=date(2024, 12, 31),
)

# 4. 分析结果
analyzer = PerformanceAnalyzer()
metrics = analyzer.analyze(portfolio.get_nav_dataframe(), benchmark_series=None)
print(f"年化收益: {metrics.annual_return:.2%}")
print(f"夏普比率: {metrics.sharpe_ratio:.2f}")
print(f"最大回撤: {metrics.max_drawdown:.2%}")
```

---

> **文档版本历史**
>
> | 版本 | 日期 | 说明 |
> |------|------|------|
> | v0.1 | 2026-05-15 | 初版：7大模块完整架构设计 |