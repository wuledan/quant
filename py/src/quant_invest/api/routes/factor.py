#!/usr/bin/env python3
"""因子计算路由."""

from __future__ import annotations

from fastapi import APIRouter

from ..schemas import FactorCalcRequest

router = APIRouter()


@router.post("/calculate")
async def calculate_factor(request: FactorCalcRequest):
    """因子计算."""
    return {"factors": {}}


@router.get("/list")
async def list_factors():
    """因子列表."""
    return {"factors": []}


@router.get("/{name}/info")
async def get_factor_info(name: str):
    """因子详情."""
    return {"name": name, "description": ""}
