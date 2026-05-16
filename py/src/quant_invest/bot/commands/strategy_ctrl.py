#!/usr/bin/env python3
"""策略控制指令处理器."""

from __future__ import annotations

from .base import CommandHandler


class StrategyCommand(CommandHandler):
    """策略控制指令.

    格式:
    /策略 → 策略列表
    /策略 启动 策略A → 启动策略
    /策略 停止 策略A → 停止策略
    /策略 参数 策略A → 查看参数
    """

    async def handle(self, text: str, event: dict) -> dict:
        """处理策略指令."""
        args = text.split()[1:] if len(text.split()) > 1 else []

        if not args:
            return {"success": True, "message": "运行中的策略: 无", "strategies": []}

        action = args[0]
        name = args[1] if len(args) > 1 else ""

        if action == "启动":
            return {"success": True, "message": f"策略 '{name}' 已启动"}
        elif action == "停止":
            return {"success": True, "message": f"策略 '{name}' 已停止"}
        elif action == "参数":
            return {"success": True, "message": f"策略 '{name}' 参数查询", "params": {}}
        else:
            return {"success": False, "error": f"未知操作: {action}"}
