#!/usr/bin/env python3
"""策略管理路由.

端点:
- GET    /list                        列出所有策略
- POST   /create                     创建新策略
- GET    /{strategy_id}               获取策略配置
- PUT    /{strategy_id}               更新策略配置
- POST   /{strategy_id}/start        启动策略
- POST   /{strategy_id}/stop         停止策略
- GET    /{strategy_id}/status        查询策略状态
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Annotated

from fastapi import APIRouter, Depends, HTTPException, status

from ..auth import AuthUser, require_auth
from ..schemas import (
    StrategyCreate,
    StrategyListResponse,
    StrategyResponse,
    StrategyStatus,
    StrategyStatusResponse,
    StrategyUpdate,
)

router = APIRouter()

# ── 模拟数据 ──

_mock_strategies: dict[str, StrategyResponse] = {
    "strat-001": StrategyResponse(
        id="strat-001",
        name="动量策略-20日",
        type="momentum",
        params={"lookback": 20, "top_n": 10},
        status=StrategyStatus.STOPPED,
        description="20日动量排名策略",
        created_at=datetime(2024, 1, 15, 8, 0, 0, tzinfo=timezone.utc),
        updated_at=datetime(2024, 6, 1, 10, 30, 0, tzinfo=timezone.utc),
    ),
    "strat-002": StrategyResponse(
        id="strat-002",
        name="均线交叉策略",
        type="ma_cross",
        params={"fast_period": 5, "slow_period": 20},
        status=StrategyStatus.RUNNING,
        description="5/20均线交叉",
        created_at=datetime(2024, 3, 10, 9, 0, 0, tzinfo=timezone.utc),
        updated_at=datetime(2024, 6, 5, 14, 0, 0, tzinfo=timezone.utc),
    ),
}


def _next_id() -> str:
    """生成下一个策略ID."""
    max_num = max(
        (int(k.split("-")[1]) for k in _mock_strategies if k.startswith("strat-")),
        default=0,
    )
    return f"strat-{max_num + 1:03d}"


@router.get("/list", response_model=StrategyListResponse)
async def list_strategies(
    user: Annotated[AuthUser, Depends(require_auth)] = None,  # noqa: B008
) -> StrategyListResponse:
    """列出所有策略."""
    # 兼容: 未注入 auth 时不阻断
    strategies = list(_mock_strategies.values())
    return StrategyListResponse(strategies=strategies, total=len(strategies))


@router.post("/create", response_model=StrategyResponse, status_code=status.HTTP_201_CREATED)
async def create_strategy(
    req: StrategyCreate,
) -> StrategyResponse:
    """创建新策略."""
    strategy_id = _next_id()
    now = datetime.now(timezone.utc)
    resp = StrategyResponse(
        id=strategy_id,
        name=req.name,
        type=req.type,
        params=req.params,
        status=StrategyStatus.CREATED,
        description=req.description,
        created_at=now,
        updated_at=now,
    )
    _mock_strategies[strategy_id] = resp
    return resp


@router.get("/{strategy_id}", response_model=StrategyResponse)
async def get_strategy(strategy_id: str) -> StrategyResponse:
    """获取策略配置详情."""
    if strategy_id not in _mock_strategies:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Strategy '{strategy_id}' not found",
        )
    return _mock_strategies[strategy_id]


@router.put("/{strategy_id}", response_model=StrategyResponse)
async def update_strategy(
    strategy_id: str,
    req: StrategyUpdate,
) -> StrategyResponse:
    """更新策略配置."""
    if strategy_id not in _mock_strategies:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Strategy '{strategy_id}' not found",
        )
    existing = _mock_strategies[strategy_id]
    update_data = req.model_dump(exclude_unset=True)
    for key, value in update_data.items():
        if hasattr(existing, key):
            setattr(existing, key, value)
    existing.updated_at = datetime.now(timezone.utc)
    return existing


@router.post("/{strategy_id}/start", response_model=StrategyStatusResponse)
async def start_strategy(strategy_id: str) -> StrategyStatusResponse:
    """启动策略."""
    if strategy_id not in _mock_strategies:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Strategy '{strategy_id}' not found",
        )
    strat = _mock_strategies[strategy_id]
    if strat.status == StrategyStatus.RUNNING:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Strategy is already running",
        )
    strat.status = StrategyStatus.RUNNING
    strat.updated_at = datetime.now(timezone.utc)
    return StrategyStatusResponse(
        strategy_id=strategy_id,
        status=StrategyStatus.RUNNING,
        started_at=datetime.now(timezone.utc),
        message="Strategy started",
    )


@router.post("/{strategy_id}/stop", response_model=StrategyStatusResponse)
async def stop_strategy(strategy_id: str) -> StrategyStatusResponse:
    """停止策略."""
    if strategy_id not in _mock_strategies:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Strategy '{strategy_id}' not found",
        )
    strat = _mock_strategies[strategy_id]
    if strat.status == StrategyStatus.STOPPED:
        raise HTTPException(
            status_code=status.HTTP_409_CONFLICT,
            detail="Strategy is already stopped",
        )
    strat.status = StrategyStatus.STOPPED
    strat.updated_at = datetime.now(timezone.utc)
    return StrategyStatusResponse(
        strategy_id=strategy_id,
        status=StrategyStatus.STOPPED,
        message="Strategy stopped",
    )


@router.get("/{strategy_id}/status", response_model=StrategyStatusResponse)
async def get_strategy_status(strategy_id: str) -> StrategyStatusResponse:
    """查询策略运行状态."""
    if strategy_id not in _mock_strategies:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Strategy '{strategy_id}' not found",
        )
    strat = _mock_strategies[strategy_id]
    return StrategyStatusResponse(
        strategy_id=strategy_id,
        status=strat.status,
        message=f"Strategy is {strat.status.value}",
    )