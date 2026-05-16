#!/usr/bin/env python3
"""飞书机器人核心.

处理流程:
1. 接收飞书事件推送 (POST /feishu/webhook)
2. 验证签名
3. 解析事件类型 (im.message.receive_v1)
4. 路由到对应指令处理器
5. 返回响应
"""

from __future__ import annotations

import hashlib
import hmac
import json
from typing import Any


class CommandParser:
    """指令解析器.

    解析用户消息，提取指令关键字和参数。
    支持格式: /关键字 [参数...] 或 关键字 [参数...]
    """

    def __init__(self) -> None:
        self._handlers: dict[str, Any] = {}

    def register(self, keyword: str, handler: Any) -> None:
        """注册指令处理器."""
        self._handlers[keyword] = handler

    def parse(self, text: str) -> tuple[str, list[str]]:
        """解析指令，返回 (keyword, args)."""
        text = text.strip()
        # 去掉开头的 /
        if text.startswith("/"):
            text = text[1:]

        parts = text.split()
        if not parts:
            return ("", [])

        keyword = parts[0]
        args = parts[1:]
        return (keyword, args)

    async def dispatch(self, text: str, event: dict) -> dict:
        """解析并分发指令."""
        keyword, args = self.parse(text)

        if keyword in self._handlers:
            return await self._handlers[keyword].handle(text, event)

        # 如果未识别，尝试帮助或默认处理器
        if "帮助" in self._handlers or "help" in self._handlers:
            handler = self._handlers.get("帮助") or self._handlers.get("help")
            return await handler.handle(text, event)

        return {"success": False, "error": f"Unknown command: {keyword}"}


class FeishuBot:
    """飞书机器人核心."""

    def __init__(
        self,
        app_id: str,
        app_secret: str,
        verification_token: str,
        encrypt_key: str | None = None,
    ) -> None:
        self.app_id = app_id
        self.app_secret = app_secret
        self.verification_token = verification_token
        self.encrypt_key = encrypt_key
        self._running = False
        self.parser = CommandParser()

    async def start(self) -> None:
        """启动机器人."""
        self._running = True

    async def stop(self) -> None:
        """停止机器人."""
        self._running = False

    @property
    def is_running(self) -> bool:
        return self._running

    async def handle_event(self, event: dict) -> dict:
        """处理飞书事件.

        支持的事件类型:
        - im.message.receive_v1: 接收用户消息
        - url_verification: URL验证挑战
        """
        # URL验证挑战
        if event.get("type") == "url_verification":
            return {"challenge": event.get("challenge", "")}

        # 事件回调
        event_body = event.get("event", {})
        msg_type = event_body.get("msg_type", "")
        msg_content = event_body.get("message", {}).get("content", "")

        if msg_type == "text":
            # 解析文本消息内容
            try:
                content_data = json.loads(msg_content) if isinstance(msg_content, str) else {}
                text = content_data.get("text", "")
            except (json.JSONDecodeError, TypeError):
                text = str(msg_content)

            return await self.parser.dispatch(text, event)

        return {"success": True}

    def verify_signature(
        self, body: bytes, signature: str, timestamp: str = "", nonce: str = ""
    ) -> bool:
        """验证飞书事件签名.

        Args:
            body: 请求体原始字节
            signature: 请求头 X-Lark-Signature
            timestamp: 请求头 X-Lark-Request-Timestamp
            nonce: 请求头 X-Lark-Request-Nonce

        Returns:
            签名是否通过
        """
        if not self.verification_token:
            return True

        if self.encrypt_key:
            # 加密模式: timestamp + nonce + encrypt_key
            raw = f"{timestamp}{nonce}{self.encrypt_key}".encode("utf-8")
        else:
            # 普通模式: verification_token + body
            raw = self.verification_token + body.decode("utf-8", errors="ignore")
            if isinstance(raw, str):
                raw = raw.encode("utf-8")

        expected = hashlib.sha256(raw).hexdigest()
        return hmac.compare_digest(expected, signature)

    def register_handler(self, keyword: str, handler: Any) -> None:
        """注册指令处理器."""
        self.parser.register(keyword, handler)
