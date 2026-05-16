#!/usr/bin/env python3
"""帮助指令处理器."""

from __future__ import annotations

from .base import CommandHandler


class HelpCommand(CommandHandler):
    """帮助指令."""

    async def handle(self, text: str, event: dict) -> dict:
        """处理帮助指令."""
        args = text.split()[1:] if len(text.split()) > 1 else []
        topic = args[0] if args else ""

        if topic:
            help_map = {
                "持仓": "查询当前持仓",
                "收益": "查询收益情况",
                "策略": "策略管理",
                "回测": "回测管理",
                "因子": "因子查询",
            }
            help_text = help_map.get(topic, f"未找到 '{topic}' 的帮助信息")
            return {"success": True, "message": help_text}

        help_text = (
            "可用指令：\n"
            "  /持仓 - 查询持仓\n"
            "  /收益 - 收益分析\n"
            "  /策略 - 策略管理\n"
            "  /回测 - 回测任务\n"
            "  /因子 - 因子查询\n"
            "  /帮助 - 显示此帮助"
        )
        return {"success": True, "message": help_text}
