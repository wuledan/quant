#!/usr/bin/env python3
"""因子计算路由.

端点:
- POST /calculate   计算因子
- GET  /list        列出可用因子
- GET  /{name}/info 因子详情
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, status

from ..schemas import FactorCalcRequest, FactorCalcResponse, FactorInfo, FactorListResponse

router = APIRouter()

_mock_factors: list[FactorInfo] = [
    FactorInfo(name="momentum_20d", category="momentum", description="20日动量因子"),
    FactorInfo(name="ma_cross_5_20", category="technical", description="5/20均线交叉"),
    FactorInfo(name="volatility_20d", category="risk", description="20日波动率"),
    FactorInfo(name="turnover_rate", category="liquidity", description="换手率"),
    FactorInfo(name="pe_ratio", category="fundamental", description="市盈率"),
    FactorInfo(name="pb_ratio", category="fundamental", description="市净率"),
]


@router.post("/calculate", response_model=FactorCalcResponse)
async def calculate_factor(req: FactorCalcRequest) -> FactorCalcResponse:
    """计算因子（mock返回随机值）."""
    import random
    factors: dict[str, dict[str, float]] = {}
    for name in req.factor_names:
        factors[name] = {s: round(random.uniform(-1, 1), 4) for s in req.symbols}
    return FactorCalcResponse(factors=factors)


@router.get("/list", response_model=FactorListResponse)
async def list_factors() -> FactorListResponse:
    """列出可用因子."""
    return FactorListResponse(factors=_mock_factors, total=len(_mock_factors))


@router.get("/{name}/info", response_model=FactorInfo)
async def get_factor_info(name: str) -> FactorInfo:
    """因子详情."""
    for f in _mock_factors:
        if f.name == name:
            return f
    raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail=f"Factor '{name}' not found")
