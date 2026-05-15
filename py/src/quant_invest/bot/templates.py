#!/usr/bin/env python3
"""飞书消息模板管理."""

from __future__ import annotations


class MessageTemplates:
    """飞书卡片消息模板."""

    @staticmethod
    def format_portfolio_card(positions: list[dict]) -> dict:
        """持仓卡片消息."""
        elements = []
        for pos in positions:
            pnl_color = "green" if pos.get("pnl_pct", 0) >= 0 else "red"
            elements.append(
                {
                    "tag": "column_set",
                    "columns": [
                        {
                            "tag": "col",
                            "width": "weighted",
                            "weight": 1,
                            "elements": [
                                {
                                    "tag": "markdown",
                                    "content": f"**{pos['symbol']}** {pos.get('name', '')}",
                                }
                            ],
                        },
                        {
                            "tag": "col",
                            "width": "weighted",
                            "weight": 1,
                            "elements": [
                                {
                                    "tag": "markdown",
                                    "content": f"<font color='{pnl_color}'>{pos['pnl_pct']:+.2f}%</font>",
                                }
                            ],
                        },
                    ],
                }
            )
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📊 当前持仓"}},
                "elements": elements,
            },
        }

    @staticmethod
    def format_trade_signal_card(signal: dict) -> dict:
        """交易信号卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📈 交易信号"}},
                "elements": [{"tag": "markdown", "content": str(signal)}],
            },
        }

    @staticmethod
    def build_push_card(message) -> dict:
        """构建推送卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": message.title}},
                "elements": [{"tag": "markdown", "content": str(message.content)}],
            },
        }
