#!/usr/bin/env python3
"""持仓组合路由.

端点:
- GET  /{portfolio_id}              获取组合快照
- GET  /{portfolio_id}/positions    列出持仓
- GET  /{portfolio_id}/pnl          获取盈亏
- GET  /{portfolio_id}/performance  绩效分析
"""

from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, status

from ..schemas import PortfolioPnLResponse, PortfolioSnapshot, PositionInfo

router = APIRouter()

_mock_portfolios: dict[str, dict] = {
    "port-001": {
        "total_value": 1_152_300.0,
        "cash": 200_000.0,
        "positions_value": 952_300.0,
        "positions": [
            PositionInfo(
                symbol="000001.SZ", quantity=1000, avg_cost=10.20,
                current_price=10.50, market_value=10_500.0, pnl=300.0, pnl_pct=0.0294,
            ),
            PositionInfo(
                symbol="600519.SH", quantity=100, avg_cost=1800.0,
                current_price=1850.0, market_value=185_000.0, pnl=5000.0, pnl_pct=0.0278,
            ),
        ],
    },
}


@router.get("/{portfolio_id}", response_model=PortfolioSnapshot)
async def get_portfolio(portfolio_id: str) -> PortfolioSnapshot:
    """获取组合快照."""
    if portfolio_id not in _mock_portfolios:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Portfolio not found")
    p = _mock_portfolios[portfolio_id]
    return PortfolioSnapshot(
        portfolio_id=portfolio_id,
        total_value=p["total_value"],
        cash=p["cash"],
        positions_value=p["positions_value"],
        positions=p["positions"],
        updated_at=datetime.now(timezone.utc),
    )


@router.get("/{portfolio_id}/positions", response_model=list[PositionInfo])
async def get_positions(portfolio_id: str) -> list[PositionInfo]:
    """列出持仓."""
    if portfolio_id not in _mock_portfolios:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Portfolio not found")
    return _mock_portfolios[portfolio_id]["positions"]


@router.get("/{portfolio_id}/pnl", response_model=PortfolioPnLResponse)
async def get_pnl(portfolio_id: str) -> PortfolioPnLResponse:
    """获取组合盈亏."""
    if portfolio_id not in _mock_portfolios:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Portfolio not found")
    return PortfolioPnLResponse(
        portfolio_id=portfolio_id,
        total_pnl=5300.0,
        realized_pnl=1200.0,
        unrealized_pnl=4100.0,
        daily_pnl=850.0,
    )


@router.get("/{portfolio_id}/performance")
async def get_performance(portfolio_id: str) -> dict:
    """绩效分析."""
    if portfolio_id not in _mock_portfolios:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Portfolio not found")
    return {
        "portfolio_id": portfolio_id,
        "total_return": 0.1523,
        "annual_return": 0.2876,
        "max_drawdown": -0.0832,
        "sharpe_ratio": 1.56,
        "win_rate": 0.62,
    }
