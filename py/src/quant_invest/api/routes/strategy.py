#!/usr/bin/env python3
"""策略管理路由."""

from __future__ import annotations

from fastapi import APIRouter

router = APIRouter()


@router.get("/list")
async def list_strategies():
    """策略列表."""
    return {"strategies": []}


@router.post("/start")
async def start_strategy(strategy_id: str):
    """启动策略."""
    return {"strategy_id": strategy_id, "status": "running"}


@router.post("/stop")
async def stop_strategy(strategy_id: str):
    """停止策略."""
    return {"strategy_id": strategy_id, "status": "stopped"}


@router.get("/{strategy_id}")
async def get_strategy(strategy_id: str):
    """策略详情."""
    return {"strategy_id": strategy_id}
