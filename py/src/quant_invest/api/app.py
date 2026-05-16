#!/usr/bin/env python3
"""FastAPI应用工厂."""

from __future__ import annotations

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware

from .routes import backtest, data, factor, portfolio, risk, strategy, system
from .ws import WebSocketManager


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

    # WebSocket管理器
    ws_manager = WebSocketManager()
    app.state.ws_manager = ws_manager

    # 注册路由
    app.include_router(data.router, prefix="/api/v1/data", tags=["数据查询"])
    app.include_router(strategy.router, prefix="/api/v1/strategy", tags=["策略管理"])
    app.include_router(backtest.router, prefix="/api/v1/backtest", tags=["回测"])
    app.include_router(portfolio.router, prefix="/api/v1/portfolio", tags=["持仓组合"])
    app.include_router(factor.router, prefix="/api/v1/factor", tags=["因子"])
    app.include_router(risk.router, prefix="/api/v1/risk", tags=["风控"])
    app.include_router(system.router, prefix="/api/v1/system", tags=["系统"])

    # WebSocket路由
    @app.websocket("/ws/{channel}")
    async def websocket_endpoint(websocket: WebSocket, channel: str):
        """WebSocket连接端点.

        支持频道:
        - market: 实时行情推送
        - portfolio: 持仓变动推送
        - risk: 风控告警推送
        """
        await ws_manager.connect(channel, websocket)
        try:
            while True:
                data = await websocket.receive_json()
                # 处理客户端请求（如订阅特定标的）
                if data.get("action") == "subscribe":
                    await websocket.send_json(
                        {"type": "subscribed", "channel": channel, "symbols": data.get("symbols", [])}
                    )
        except WebSocketDisconnect:
            await ws_manager.disconnect(channel, websocket)

    @app.get("/health")
    async def health_check() -> dict:
        return {"status": "ok"}

    return app


app = create_app()