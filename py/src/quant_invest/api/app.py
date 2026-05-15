#!/usr/bin/env python3
"""FastAPI应用工厂."""

from __future__ import annotations

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware


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

    @app.get("/health")
    async def health_check() -> dict:
        return {"status": "ok"}

    return app


app = create_app()
