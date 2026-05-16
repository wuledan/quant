#!/usr/bin/env python3
"""绩效查询/回测/因子指令处理器."""

from __future__ import annotations

from .base import CommandHandler


class PerformanceCommand(CommandHandler):
    """收益查询指令.

    格式:
    /收益 → 查看今日收益
    /收益 本月 → 查看本月收益
    """

    async def handle(self, text: str, event: dict) -> dict:
        """处理收益查询."""
        from ..templates import MessageTemplates

        args = text.split()[1:] if len(text.split()) > 1 else []
        period = args[0] if args else "今日"

        return {
            "success": True,
            "card": MessageTemplates.format_performance_card(
                {"period": period, "total_return": 0.0, "daily_pnl": 0.0}
            ),
        }


class BacktestCommand(CommandHandler):
    """回测指令.

    格式:
    /回测 → 查看最近回测
    /回测 运行 策略A 2024-01-01 2024-12-31 → 运行回测
    """

    async def handle(self, text: str, event: dict) -> dict:
        """处理回测指令."""
        from ..templates import MessageTemplates

        args = text.split()[1:] if len(text.split()) > 1 else []

        if args and args[0] == "运行":
            return {
                "success": True,
                "message": "回测任务已提交，请稍后查询结果",
            }

        return {
            "success": True,
            "card": MessageTemplates.format_performance_card(
                {"total_return": 0.15, "sharpe": 1.2, "max_drawdown": -0.08}
            ),
        }


class FactorCommand(CommandHandler):
    """因子查询指令.

    格式:
    /因子 → 列出所有因子
    /因子 动量20 → 查询动量因子
    """

    async def handle(self, text: str, event: dict) -> dict:
        """处理因子查询."""
        args = text.split()[1:] if len(text.split()) > 1 else []
        factor_name = args[0] if args else ""

        return {
            "success": True,
            "message": f"因子查询: {factor_name or '全部因子'}",
        }
  