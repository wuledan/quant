"""API依赖注入."""

from __future__ import annotations

from fastapi import Request


def get_settings(request: Request):
    """从请求状态获取Settings."""
    return request.app.state.settings


def get_ws_manager(request: Request):
    """获取WebSocket管理器."""
    return request.app.state.ws_manager
