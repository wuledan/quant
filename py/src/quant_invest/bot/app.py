#!/usr/bin/env python3
"""飞书机器人核心."""

from __future__ import annotations


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
        self.command_handlers: dict[str, object] = {}

    async def handle_event(self, event: dict) -> dict:
        """处理飞书事件."""
        return {"success": True}
