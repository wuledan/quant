#!/usr/bin/env python3
"""回测任务路由 — 对接 BacktestTaskManager."""

from __future__ import annotations

from datetime import date

from fastapi import APIRouter, HTTPException, Query, status

from ..schemas import BacktestRequest, BacktestResultResponse, BacktestStatusResponse
from ...backtest.task_manager import get_backtest_manager, TaskStatus

router = APIRouter()


@router.post("/run", status_code=status.HTTP_202_ACCEPTED)
async def run_backtest(req: BacktestRequest) -> dict:
    """提交回测任务."""
    if req.end_date <= req.start_date:
        raise HTTPException(status_code=422, detail="end_date must be after start_date")
    if req.initial_cash <= 0:
        raise HTTPException(status_code=422, detail="initial_cash must be positive")

    # 从策略ID获取策略类型
    strategy_type = _resolve_strategy_type(req.strategy_id)

    manager = get_backtest_manager()
    task_id = manager.submit(
        strategy_id=req.strategy_id,
        strategy_type=strategy_type,
        symbols=req.symbols,
        start_date=req.start_date,
        end_date=req.end_date,
        initial_cash=req.initial_cash,
    )
    return {"backtest_id": task_id, "status": "pending"}


@router.get("/{task_id}/status", response_model=BacktestStatusResponse)
async def get_backtest_status(task_id: str) -> BacktestStatusResponse:
    """查询回测进度."""
    manager = get_backtest_manager()
    info = manager.get_status(task_id)
    if info is None:
        raise HTTPException(status_code=404, detail=f"Task '{task_id}' not found")
    return BacktestStatusResponse(
        task_id=info["task_id"],
        status=info["status"],
        progress=info["progress"],
        message=info["message"],
    )


@router.get("/{task_id}/result", response_model=BacktestResultResponse)
async def get_backtest_result(task_id: str) -> BacktestResultResponse:
    """获取回测结果."""
    manager = get_backtest_manager()
    result = manager.get_result(task_id)
    if result is None:
        # 任务可能还在运行
        info = manager.get_status(task_id)
        if info is None:
            raise HTTPException(status_code=404, detail=f"Task '{task_id}' not found")
        if info["status"] in ("pending", "running"):
            raise HTTPException(status_code=202, detail="回测尚未完成")
        if info["status"] == "failed":
            raise HTTPException(status_code=500, detail=info["message"])
        raise HTTPException(status_code=404, detail="结果不可用")
    return BacktestResultResponse(**result)


@router.get("/list")
async def list_backtests(
    limit: int = Query(20, ge=1, le=100),
) -> dict:
    """列出回测任务."""
    manager = get_backtest_manager()
    tasks = manager.list_tasks(limit=limit)
    return {"backtests": tasks, "total": len(tasks)}


def _resolve_strategy_type(strategy_id: str) -> str:
    """从策略ID解析策略类型.

    优先从策略注册表匹配，fallback到ID前缀。
    """
    from ...strategy.registry import StrategyRegistry

    # 1. 直接匹配注册名
    registered = StrategyRegistry.list_strategies()
    if strategy_id in registered:
        return strategy_id

    # 2. 从策略管理路由的mock数据中查
    # TODO: 接入持久化策略存储后替换
    type_map = {
        "strat-001": "ma_cross",
        "strat-002": "momentum",
    }
    if strategy_id in type_map:
        return type_map[strategy_id]

    # 3. 尝试把整个ID当策略类型
    if strategy_id in registered:
        return strategy_id

    # 4. 默认ma_cross
    return "ma_cross"
