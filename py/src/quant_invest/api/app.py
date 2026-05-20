#!/usr/bin/env python3
"""FastAPI应用工厂."""

from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware

from .routes import backtest, data, factor, news, portfolio, risk, strategy, system
from .ws import WebSocketManager
from ..data.scheduler import get_scheduler
from ..backtest.task_manager import get_backtest_manager

logger = logging.getLogger("quant_invest.api")


@asynccontextmanager
async def lifespan(app: FastAPI):
    """应用生命周期管理."""
    # Startup
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    )
    logger.info("QuantInvest API 启动中...")

    # 启动数据调度服务
    scheduler = get_scheduler()
    scheduler_task = asyncio.create_task(scheduler.run_forever())
    app.state.scheduler = scheduler
    app.state.scheduler_task = scheduler_task
    logger.info("数据调度服务已启动")

    # 启动回测任务管理器
    bt_manager = get_backtest_manager()
    app.state.backtest_manager = bt_manager
    logger.info("回测任务管理器已启动 (max_workers=2)")

    yield

    # Shutdown
    bt_manager.shutdown()
    scheduler.stop()
    scheduler_task.cancel()
    try:
        await scheduler_task
    except asyncio.CancelledError:
        pass
    logger.info("QuantInvest API 已关闭")


def create_app() -> FastAPI:
    """创建FastAPI应用实例."""
    app = FastAPI(
        title="QuantInvest API",
        version="0.1.0",
        description="A股量化投资系统API",
        lifespan=lifespan,
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
    app.include_router(news.router, prefix="/api/v1/news", tags=["新闻"])
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
