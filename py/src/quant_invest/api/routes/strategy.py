#!/usr/bin/env python3
"""策略管理路由."""

from __future__ import annotations

from fastapi import APIRouter

router = APIRouter()


@router.get("/list")
async def list_strategies():
    """策略列表."""
    return {"strategies": []}


@router.get("/{strategy_id}")
async def get_strategy(strategy_id: str):
    """策略详情."""
    return {"strategy_id": strategy_id}
