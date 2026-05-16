#!/usr/bin/env python3
"""回测任务路由."""

from __future__ import annotations

from fastapi import APIRouter, BackgroundTasks, Query

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


@router.get("/compare")
async def compare_backtests(
    backtest_ids: str = Query(..., description="逗号分隔的回测ID"),
):
    """对比多个回测结果."""
    ids = backtest_ids.split(",")
    return {
        "backtest_ids": ids,
        "comparison": {},
        "metrics_diff": {},
    }


@router.get("/list")
async def list_backtests(
    strategy_id: str = "",
    status: str = "",
    limit: int = 20,
):
    """列出回测任务."""
    return {"backtests": []}