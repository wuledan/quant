#!/usr/bin/env python3
"""回测任务路由."""

from __future__ import annotations

from fastapi import APIRouter, BackgroundTasks

from ..schemas import BacktestRequest

router = APIRouter()


@router.post("/run")
async def run_backtest(
    request: BacktestRequest,
    background_tasks: BackgroundTasks,
):
    """提交回测任务（异步执行）."""
    backtest_id = "temp_id"
    return {"backtest_id": backtest_id, "status": "pending"}


@router.get("/result/{backtest_id}")
async def get_backtest_result(backtest_id: str):
    """查询回测结果."""
    return {"backtest_id": backtest_id, "status": "pending"}


@router.get("/list")
async def list_backtests(
    strategy_id: str = "",
    status: str = "",
    limit: int = 20,
):
    """列出回测任务."""
    return {"backtests": []}
