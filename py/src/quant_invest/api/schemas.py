"""Pydantic请求/响应模型."""

from __future__ import annotations

from datetime import date, datetime
from enum import Enum

from pydantic import BaseModel, Field


# ── 枚举 ──

class StrategyStatus(str, Enum):
    """策略运行状态."""

    CREATED = "created"
    RUNNING = "running"
    STOPPED = "stopped"
    ERROR = "error"


class RiskRuleSeverity(str, Enum):
    """风控规则严重级别."""

    LOW = "low"
    MEDIUM = "medium"
    HIGH = "high"


class UserRole(str, Enum):
    """用户角色."""

    ADMIN = "admin"
    READER = "reader"


# ── 策略 ──

class StrategyCreate(BaseModel):
    """创建策略请求."""

    name: str = Field(..., min_length=1, max_length=100, description="策略名称")
    type: str = Field(..., min_length=1, description="策略类型")
    params: dict = Field(default_factory=dict, description="策略参数")
    description: str = Field("", max_length=500, description="策略描述")


class StrategyUpdate(BaseModel):
    """更新策略请求."""

    name: str | None = Field(None, min_length=1, max_length=100, description="策略名称")
    type: str | None = Field(None, min_length=1, description="策略类型")
    params: dict | None = Field(None, description="策略参数")
    description: str | None = Field(None, max_length=500, description="策略描述")


class StrategyResponse(BaseModel):
    """策略信息响应."""

    id: str
    name: str
    type: str
    params: dict
    status: StrategyStatus
    description: str = ""
    created_at: datetime
    updated_at: datetime


class StrategyListResponse(BaseModel):
    """策略列表响应."""

    strategies: list[StrategyResponse]
    total: int


class StrategyStatusResponse(BaseModel):
    """策略状态响应."""

    strategy_id: str
    status: StrategyStatus
    started_at: datetime | None = None
    message: str = ""


# ── 回测 ──

class BacktestRequest(BaseModel):
    """回测请求."""

    strategy_id: str = Field(..., description="策略ID")
    symbols: list[str] = Field(..., min_length=1, description="标的列表")
    start_date: date = Field(..., description="开始日期")
    end_date: date = Field(..., description="结束日期")
    frequency: str = Field("daily", description="数据频率")
    initial_cash: float = Field(1_000_000, gt=0, description="初始资金")


class BacktestResultResponse(BaseModel):
    """回测结果响应."""

    backtest_id: str
    status: str
    strategy_id: str = ""
    metrics: dict | None = None
    nav_curve: list[dict] | None = None
    trades: list[dict] | None = None


class BacktestStatusResponse(BaseModel):
    """回测状态响应."""

    task_id: str
    status: str
    progress: float = 0.0
    message: str = ""


# ── 持仓组合 ──

class PositionInfo(BaseModel):
    """持仓信息."""

    symbol: str
    quantity: int
    avg_cost: float
    current_price: float = 0.0
    market_value: float = 0.0
    pnl: float = 0.0
    pnl_pct: float = 0.0


class PortfolioSnapshot(BaseModel):
    """组合快照."""

    portfolio_id: str
    total_value: float
    cash: float
    positions_value: float = 0.0
    positions: list[PositionInfo] = Field(default_factory=list)
    updated_at: datetime


class PortfolioPnLResponse(BaseModel):
    """组合盈亏响应."""

    portfolio_id: str
    total_pnl: float = 0.0
    realized_pnl: float = 0.0
    unrealized_pnl: float = 0.0
    daily_pnl: float = 0.0
    positions_pnl: list[dict] = Field(default_factory=list)


# ── 因子 ──

class FactorCalcRequest(BaseModel):
    """因子计算请求."""

    factor_names: list[str]
    symbols: list[str]
    date: date


class FactorCalcResponse(BaseModel):
    """因子计算响应."""

    factors: dict[str, dict[str, float]]


class FactorInfo(BaseModel):
    """因子信息."""

    name: str
    category: str = ""
    description: str = ""


class FactorListResponse(BaseModel):
    """因子列表响应."""

    factors: list[FactorInfo]
    total: int


# ── 风控 ──

class RiskAlertCreate(BaseModel):
    """风控告警创建请求."""

    alert_type: str = Field(..., min_length=1, description="告警类型")
    severity: RiskRuleSeverity = Field(RiskRuleSeverity.MEDIUM, description="告警级别")
    message: str = Field(..., min_length=1, description="告警信息")
    symbol: str = Field("", description="关联标的")


class RiskRuleInfo(BaseModel):
    """风控规则信息."""

    rule_id: str
    name: str
    description: str = ""
    severity: RiskRuleSeverity = RiskRuleSeverity.MEDIUM
    enabled: bool = True
    params: dict = Field(default_factory=dict)


class RiskCheckResponse(BaseModel):
    """风控检查响应."""

    passed: bool
    violations: list[dict] = Field(default_factory=list)
    risk_level: str = "low"


class RiskStatusResponse(BaseModel):
    """风控状态响应."""

    overall_status: str = "normal"
    risk_level: str = "low"
    active_rules: int = 0
    recent_alerts: list[dict] = Field(default_factory=list)


# ── 数据 ──

class DataKlineRequest(BaseModel):
    """K线数据请求."""

    symbol: str = Field(..., description="标的代码")
    frequency: str = Field("daily", description="数据频率")
    start_date: str = Field("", description="开始日期")
    end_date: str = Field("", description="结束日期")
    adjust: str = Field("none", description="复权方式")
    limit: int = Field(1000, description="返回条数", le=10000)


class DailyBarResponse(BaseModel):
    """日线数据响应."""

    symbol: str
    data: list[dict] = Field(default_factory=list)
    count: int = 0


class MinuteBarResponse(BaseModel):
    """分钟线数据响应."""

    symbol: str
    date: str = ""
    data: list[dict] = Field(default_factory=list)
    count: int = 0


class FinancialDataResponse(BaseModel):
    """财务数据响应."""

    symbol: str
    report_type: str = "income"
    data: dict = Field(default_factory=dict)


class MacroDataResponse(BaseModel):
    """宏观数据响应."""

    indicator: str
    data: list[dict] = Field(default_factory=list)
    count: int = 0


# ── 认证 ──

class TokenResponse(BaseModel):
    """Token响应."""

    access_token: str
    token_type: str = "bearer"
    expires_at: datetime


class UserInfo(BaseModel):
    """用户信息."""

    user_id: str
    role: UserRole
    username: str = ""