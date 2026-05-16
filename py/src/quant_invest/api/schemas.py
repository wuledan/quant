"""Pydantic请求/响应模型."""

from __future__ import annotations

from datetime import date, datetime

from pydantic import BaseModel, Field


class StrategyCreate(BaseModel):
    """创建策略请求."""

    name: str = Field(..., description="策略名称")
    type: str = Field(..., description="策略类型")
    params: dict = Field(default_factory=dict, description="策略参数")
    description: str = Field("", description="策略描述")


class StrategyResponse(BaseModel):
    """策略信息响应."""

    id: str
    name: str
    type: str
    params: dict
    status: str
    created_at: datetime
    updated_at: datetime


class BacktestRequest(BaseModel):
    """回测请求."""

    strategy_id: str = Field(..., description="策略ID")
    symbols: list[str] = Field(..., description="标的列表")
    start_date: date = Field(..., description="开始日期")
    end_date: date = Field(..., description="结束日期")
    frequency: str = Field("daily", description="数据频率")
    initial_cash: float = Field(1_000_000, description="初始资金")


class BacktestResultResponse(BaseModel):
    """回测结果响应."""

    backtest_id: str
    status: str
    metrics: dict | None = None
    nav_curve: list[dict] | None = None
    trades: list[dict] | None = None


class PortfolioSnapshot(BaseModel):
    """组合快照."""

    total_value: float
    cash: float
    positions: list[dict]
    updated_at: datetime


class FactorCalcRequest(BaseModel):
    """因子计算请求."""

    factor_names: list[str]
    symbols: list[str]
    date: date


class FactorCalcResponse(BaseModel):
    """因子计算响应."""

    factors: dict[str, dict[str, float]]


class RiskAlertCreate(BaseModel):
    """风控告警创建请求."""

    alert_type: str = Field(..., description="告警类型")
    severity: str = Field("medium", description="告警级别 high/medium/low")
    message: str = Field(..., description="告警信息")
    symbol: str = Field("", description="关联标的")


class DataKlineRequest(BaseModel):
    """K线数据请求."""

    symbol: str = Field(..., description="标的代码")
    frequency: str = Field("daily", description="数据频率")
    start_date: str = Field("", description="开始日期")
    end_date: str = Field("", description="结束日期")
    adjust: str = Field("none", description="复权方式")
    limit: int = Field(1000, description="返回条数", le=10000)