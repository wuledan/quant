#!/usr/bin/env python3
"""回测任务路由.

端点:
- POST /run                        提交回测(异步执行)
- GET  /{task_id}/status            查询回测进度
- GET  /{task_id}/result            获取回测结果(指标+净值曲线)
- GET  /compare                     对比多个回测结果
- GET  /list                        列出回测任务
"""

from __future__ import annotations

import asyncio
import uuid
from datetime import datetime, timezone
from typing import Annotated

from fastapi import APIRouter, BackgroundTasks, HTTPException, Query, status

from ..schemas import BacktestRequest, BacktestResultResponse, BacktestStatusResponse

router = APIRouter()

# ── 模拟存储 ──

_backtest_tasks: dict[str, BacktestStatusResponse] = {}
_backtest_results: dict[str, BacktestResultResponse] = {}


async def _run_backtest_mock(task_id: str, req: BacktestRequest) -> None:
    """模拟异步回测执行.

    真实实现会:
    1. 从数据源获取行情数据
    2. 创建回测引擎并运行
    3. 生成绩效报告
    """
    # 模拟回测耗时
    for progress in range(0, 101, 20):
        _backtest_tasks[task_id].progress = float(progress)
        await asyncio.sleep(0.1)

    # 生成模拟结果
    _backtest_results[task_id] = BacktestResultResponse(
        backtest_id=task_id,
        status="completed",
        strategy_id=req.strategy_id,
        metrics={
            "total_return": 0.1523,
            "annual_return": 0.2876,
            "max_drawdown": -0.0832,
            "sharpe_ratio": 1.56,
            "win_rate": 0.62,
            "total_trades": 156,
            "profit_factor": 1.85,
        },
        nav_curve=[
            {"date": "2024-01-02", "nav": 1_000_000},
            {"date": "2024-03-31", "nav": 1_082_500},
            {"date": "2024-06-30", "nav": 1_145_600},
            {"date": "2024-09-30", "nav": 1_102_300},
            {"date": "2024-12-31", "nav": 1_152_300},
        ],
        trades=[
            {
                "date": "2024-01-15",
                "symbol": "000001.SZ",
                "direction": "BUY",
                "quantity": 1000,
                "price": 10.50,
            },
        ],
    )
    _backtest_tasks[task_id].status = "completed"
    _backtest_tasks[task_id].progress = 100.0
    _backtest_tasks[task_id].message = "Backtest completed"


# ── 路由 ──

@router.post("/run", status_code=status.HTTP_202_ACCEPTED)
async def run_backtest(
    req: BacktestRequest,
    background_tasks: BackgroundTasks,
) -> dict:
    """提交回测任务(异步执行).

    Returns task_id 用于后续查询进度和结果.
    """
    # 参数校验
    if req.end_date <= req.start_date:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="end_date must be after start_date",
        )
    if req.initial_cash <= 0:
        raise HTTPException(
            status_code=status.HTTP_422_UNPROCESSABLE_ENTITY,
            detail="initial_cash must be positive",
        )

    task_id = f"bt-{uuid.uuid4().hex[:8]}"
    _backtest_tasks[task_id] = BacktestStatusResponse(
        task_id=task_id,
        status="pending",
        progress=0.0,
        message="Backtest queued",
    )
    background_tasks.add_task(_run_backtest_mock, task_id, req)
    return {"backtest_id": task_id, "status": "pending"}


@router.get("/{task_id}/status", response_model=BacktestStatusResponse)
async def get_backtest_status(task_id: str) -> BacktestStatusResponse:
    """查询回测进度."""
    if task_id not in _backtest_tasks:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Backtest task '{task_id}' not found",
        )
    return _backtest_tasks[task_id]


@router.get("/{task_id}/result", response_model=BacktestResultResponse)
async def get_backtest_result(task_id: str) -> BacktestResultResponse:
    """获取回测结果(指标 + 净值曲线)."""
    if task_id not in _backtest_results:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail=f"Backtest result '{task_id}' not found or not yet completed",
        )
    return _backtest_results[task_id]


@router.get("/compare")
async def compare_backtests(
    backtest_ids: str = Query(..., description="逗号分隔的回测ID"),
) -> dict:
    """对比多个回测结果."""
    ids = backtest_ids.split(",")
    results = []
    for bid in ids:
        if bid in _backtest_results:
            results.append(_backtest_results[bid])
    return {
        "backtest_ids": ids,
        "results": [r.model_dump() for r in results],
        "metrics_diff": {},
    }


@router.get("/list")
async def list_backtests(
    strategy_id: str = "",
    status_filter: str = Query("", alias="status"),
    limit: int = Query(20, ge=1, le=100),
) -> dict:
    """列出回测任务."""
    tasks = list(_backtest_tasks.values())
    if strategy_id:
        # 过滤 strategy_id（模拟数据中没有存储，所以不做真正过滤）
        pass
    if status_filter:
        tasks = [t for t in tasks if t.status == status_filter]
    return {"backtests": [t.model_dump() for t in tasks[:limit]], "total": len(tasks)}