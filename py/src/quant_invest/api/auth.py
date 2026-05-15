"""认证与权限中间件."""

from __future__ import annotations

from fastapi import Request
from starlette.middleware.base import BaseHTTPMiddleware


class AuthMiddleware(BaseHTTPMiddleware):
    """JWT + API Key认证中间件."""

    WHITELIST = {"/health", "/docs", "/openapi.json", "/redoc"}

    async def dispatch(self, request: Request, call_next):
        if request.url.path in self.WHITELIST:
            return await call_next(request)
        # TODO: 实现JWT/API Key验证
        return await call_next(request)
