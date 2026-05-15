#!/usr/bin/env python3
"""持仓组合路由."""

from __future__ import annotations

from fastapi import APIRouter

router = APIRouter()


@router.get("/snapshot")
async def get_snapshot():
    """当前持仓快照."""
    return {"positions": [], "total_value": 0.0, "cash": 0.0}


@router.get("/history")
async def get_history():
    """持仓历史."""
    return {"history": []}


@router.get("/performance")
async def get_performance():
    """绩效分析."""
    return {"metrics": {}}
