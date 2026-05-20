#!/usr/bin/env python3
"""策略管理路由 — 对接策略注册表 + 热重载管理."""

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

_next_id_counter = 3


def _next_id() -> str:
    global _next_id_counter
    _next_id_counter += 1
    return f"strat-{_next_id_counter:03d}"


_mock_strategies: dict[str, StrategyResponse] = {
    "strat-001": StrategyResponse(
        id="strat-001",
        name="均线交叉策略",
        type="ma_cross",
        params={"fast_period": 5, "slow_period": 20},
        status=StrategyStatus.STOPPED,
        description="5/20 双均线交叉",
        created_at=datetime(2024, 3, 10, 9, 0, 0, tzinfo=timezone.utc),
        updated_at=datetime(2025, 5, 15, 14, 0, 0, tzinfo=timezone.utc),
    ),
    "strat-002": StrategyResponse(
        id="strat-002",
        name="动量选股策略",
        type="momentum",
        params={"window": 20, "top_n": 10},
        status=StrategyStatus.STOPPED,
        description="20日动量排名选股",
        created_at=datetime(2024, 1, 15, 8, 0, 0, tzinfo=timezone.utc),
        updated_at=datetime(2025, 5, 18, 10, 30, 0, tzinfo=timezone.utc),
    ),
}


# ── 前端兼容路由 (匹配 frontend API 路径) ──

@router.get("/list", response_model=StrategyListResponse)
@router.get("",       response_model=StrategyListResponse, include_in_schema=False)
async def list_strategies() -> StrategyListResponse:
    # 合并注册表策略和 mock 策略
    from ...strategy.registry import StrategyRegistry
    registered = StrategyRegistry.list_strategies()
    strategies = list(_mock_strategies.values())

    # 把注册表中有但 mock 中没有的策略也加入
    existing_types = {s.type for s in strategies}
    for name in registered:
        if name not in existing_types:
            now = datetime.now(timezone.utc)
            strategies.append(StrategyResponse(
                id=f"strat-reg-{name}",
                name=name,
                type=name,
                params={},
                status=StrategyStatus.STOPPED,
                description=f"注册策略: {name}",
                created_at=now,
                updated_at=now,
            ))

    return StrategyListResponse(strategies=strategies, total=len(strategies))


@router.post("/create", response_model=StrategyResponse, status_code=status.HTTP_201_CREATED)
@router.post("",        response_model=StrategyResponse, status_code=status.HTTP_201_CREATED, include_in_schema=False)
async def create_strategy(req: StrategyCreate) -> StrategyResponse:
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


# ── 热重载路由（必须在 /{strategy_id} 之前注册，避免路径参数冲突）──

@router.post("/reload/{name}")
async def reload_strategy(name: str) -> dict:
    """手动触发策略热重载.

    流程:
    1. 调用 StrategyRegistry.reload(name) 重新导入策略模块
    2. 调用 HotReloadManager 重新编译策略
    3. 更新编译缓存

    Args:
        name: 策略注册名（如 "ma_cross_dsl"）
    """
    from ...strategy.hot_reload import get_hot_reload_manager
    from ...strategy.registry import StrategyRegistry

    # 检查策略是否存在
    if not StrategyRegistry.has(name):
        raise HTTPException(
            status_code=404,
            detail=f"Strategy '{name}' not found in registry. "
                   f"Available: {StrategyRegistry.list_strategies()}",
        )

    manager = get_hot_reload_manager()
    result = manager.reload_strategy(name)

    return {
        "strategy_name": result.strategy_name,
        "success": result.success,
        "compiled": result.compiled,
        "error_message": result.error_message,
        "previous_version_kept": result.previous_version_kept,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }


@router.get("/reload/status")
async def get_reload_status() -> dict:
    """获取热重载状态.

    返回:
    - 热重载管理器状态（缓存大小、编译统计等）
    - 文件监视器状态（运行状态、监视文件列表、最近重载记录）
    """
    from ...strategy.hot_reload import get_hot_reload_manager

    manager = get_hot_reload_manager()
    return manager.get_status()


# ── 策略 CRUD 路由（路径参数路由）──

@router.get("/{strategy_id}", response_model=StrategyResponse)
async def get_strategy(strategy_id: str) -> StrategyResponse:
    if strategy_id not in _mock_strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    return _mock_strategies[strategy_id]


@router.put("/{strategy_id}", response_model=StrategyResponse)
async def update_strategy(strategy_id: str, req: StrategyUpdate) -> StrategyResponse:
    if strategy_id not in _mock_strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    existing = _mock_strategies[strategy_id]
    update_data = req.model_dump(exclude_unset=True)
    for key, value in update_data.items():
        if hasattr(existing, key):
            setattr(existing, key, value)
    existing.updated_at = datetime.now(timezone.utc)
    return existing


@router.post("/{strategy_id}/run")
@router.post("/{strategy_id}/start")
async def run_strategy(strategy_id: str) -> StrategyStatusResponse:
    if strategy_id not in _mock_strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    strat = _mock_strategies[strategy_id]
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
    if strategy_id not in _mock_strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    strat = _mock_strategies[strategy_id]
    strat.status = StrategyStatus.STOPPED
    strat.updated_at = datetime.now(timezone.utc)
    return StrategyStatusResponse(
        strategy_id=strategy_id,
        status=StrategyStatus.STOPPED,
        message="Strategy stopped",
    )


@router.get("/{strategy_id}/status", response_model=StrategyStatusResponse)
async def get_strategy_status(strategy_id: str) -> StrategyStatusResponse:
    if strategy_id not in _mock_strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    strat = _mock_strategies[strategy_id]
    return StrategyStatusResponse(
        strategy_id=strategy_id,
        status=strat.status,
        message=f"Strategy is {strat.status.value}",
    )
