#!/usr/bin/env python3
"""系统管理路由."""

from __future__ import annotations

from fastapi import APIRouter

router = APIRouter()


@router.get("/health")
async def health_check():
    """健康检查."""
    return {"status": "ok", "version": "0.1.0"}


@router.get("/status")
async def system_status():
    """系统状态."""
    return {"status": "running"}
