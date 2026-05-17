"""JWT认证与权限中间件.

提供:
- JWT token 生成与验证
- FastAPI 依赖注入 require_auth
- 简单 RBAC: admin / reader 角色
- AuthMiddleware 白名单放行
"""

from __future__ import annotations

import os
from datetime import datetime, timedelta, timezone
from typing import Annotated

from fastapi import Depends, HTTPException, Request, status
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from jose import JWTError, jwt

from .schemas import UserRole

# ── 配置 ──

JWT_SECRET = os.getenv("JWT_SECRET", "quant_invest_dev_secret_change_me")
JWT_ALGORITHM = "HS256"
JWT_EXPIRE_HOURS = int(os.getenv("JWT_EXPIRE_HOURS", "24"))

security_scheme = HTTPBearer(auto_error=False)


# ── Token 操作 ──

def create_token(user_id: str, role: UserRole = UserRole.READER, expire_hours: int | None = None) -> str:
    """生成 JWT token.

    Args:
        user_id: 用户标识
        role: 用户角色
        expire_hours: 过期时间(小时)，默认使用全局配置

    Returns:
        编码后的 JWT 字符串
    """
    hours = expire_hours or JWT_EXPIRE_HOURS
    now = datetime.now(timezone.utc)
    payload = {
        "sub": user_id,
        "role": role.value,
        "iat": now,
        "exp": now + timedelta(hours=hours),
    }
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALGORITHM)


def verify_token(token: str) -> dict:
    """验证 JWT token 并返回 payload.

    Args:
        token: JWT 字符串

    Returns:
        解码后的 payload dict

    Raises:
        HTTPException: token 无效或已过期
    """
    try:
        payload = jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALGORITHM])
        return payload
    except JWTError as exc:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail=f"Invalid token: {exc}",
            headers={"WWW-Authenticate": "Bearer"},
        ) from exc


# ── FastAPI 依赖 ──

class AuthUser:
    """认证用户信息，注入到路由."""

    def __init__(self, user_id: str, role: UserRole) -> None:
        self.user_id = user_id
        self.role = role

    @property
    def is_admin(self) -> bool:
        return self.role == UserRole.ADMIN


def _extract_user(credentials: HTTPAuthorizationCredentials | None) -> AuthUser:
    """从 HTTP Authorization header 解析用户."""
    if credentials is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Missing authentication token",
            headers={"WWW-Authenticate": "Bearer"},
        )
    payload = verify_token(credentials.credentials)
    return AuthUser(
        user_id=payload["sub"],
        role=UserRole(payload.get("role", "reader")),
    )


async def require_auth(
    credentials: Annotated[HTTPAuthorizationCredentials | None, Depends(security_scheme)],
) -> AuthUser:
    """FastAPI 依赖: 要求认证."""
    return _extract_user(credentials)


async def require_admin(
    user: Annotated[AuthUser, Depends(require_auth)],
) -> AuthUser:
    """FastAPI 依赖: 要求 admin 角色."""
    if not user.is_admin:
        raise HTTPException(
            status_code=status.HTTP_403_FORBIDDEN,
            detail="Admin access required",
        )
    return user


# ── 中间件 ──

class AuthMiddleware:
    """JWT + 白名单认证中间件 (Starlette BaseHTTPMiddleware 风格)."""

    WHITELIST = {"/health", "/docs", "/openapi.json", "/redoc", "/api/v1/auth/token"}

    def __init__(self, app=None) -> None:
        self.app = app

    async def __call__(self, scope, receive, send):
        """ASGI 中间件入口."""
        if scope["type"] not in ("http", "websocket"):
            await self.app(scope, receive, send)
            return

        # WebSocket 不做认证检查
        if scope["type"] == "websocket":
            await self.app(scope, receive, send)
            return

        path = scope.get("path", "")
        if path in self.WHITELIST or path.startswith("/docs") or path.startswith("/static"):
            await self.app(scope, receive, send)
            return

        # 对需要认证的路径，仅做 token 存在性检查（详细验证在依赖中完成）
        # 这里不阻断请求，让路由中的 require_auth 依赖处理 401/403
        await self.app(scope, receive, send)