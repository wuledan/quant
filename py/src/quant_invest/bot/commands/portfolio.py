#!/usr/bin/env python3
"""持仓查询指令处理器."""

from __future__ import annotations

from .base import CommandHandler


class PortfolioCommand(CommandHandler):
    """持仓查询指令."""

    async def handle(self, text: str, event: dict) -> dict:
        """处理持仓查询."""
        return {"success": True}
