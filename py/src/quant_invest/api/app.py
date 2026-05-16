#!/usr/bin/env python3
"""FastAPI应用工厂."""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from .routes import backtest, factor, portfolio, strategy, system


def create_app() -> FastAPI:
    """创建FastAPI应用实例."""
    app = FastAPI(
        title="QuantInvest API",
        version="0.1.0",
        description="A股量化投资系统API",
    )

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # 注册路由
    app.include_router(strategy.router, prefix="/api/v1/strategy", tags=["策略管理"])
    app.include_router(backtest.router, prefix="/api/v1/backtest", tags=["回测"])
    app.include_router(portfolio.router, prefix="/api/v1/portfolio", tags=["持仓组合"])
    app.include_router(factor.router, prefix="/api/v1/factor", tags=["因子"])
    app.include_router(system.router, prefix="/api/v1/system", tags=["系统"])

    @app.get("/health")
    async def health_check() -> dict:
        return {"status": "ok"}

    return app


app = create_app()
