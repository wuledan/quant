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
    def format_portfolio_summary_card(summary: dict) -> dict:
        """持仓汇总卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📊 持仓汇总"}},
                "elements": [
                    {"tag": "markdown", "content": f"总资产: {summary.get('total_value', 0):,.2f}"},
                    {"tag": "markdown", "content": f"持仓个数: {summary.get('position_count', 0)}"},
                    {"tag": "markdown", "content": f"当日盈亏: {summary.get('daily_pnl', 0):+,.2f}"},
                ],
            },
        }

    @staticmethod
    def format_performance_card(metrics: dict) -> dict:
        """绩效指标卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📈 绩效分析"}},
                "elements": [
                    {"tag": "markdown", "content": f"总收益率: {metrics.get('total_return', 0):+.2%}"},
                    {"tag": "markdown", "content": f"夏普比率: {metrics.get('sharpe', 0):.2f}"},
                    {"tag": "markdown", "content": f"最大回撤: {metrics.get('max_drawdown', 0):.2%}"},
                ],
            },
        }

    @staticmethod
    def format_trade_signal_card(signal: dict) -> dict:
        """交易信号卡片."""
        direction_color = "green" if signal.get("direction") == "BUY" else "red"
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📈 交易信号"}},
                "elements": [
                    {
                        "tag": "markdown",
                        "content": (
                            f"标的: {signal.get('symbol', '')}\n"
                            f"方向: <font color='{direction_color}'>{signal.get('direction', '')}</font>\n"
                            f"数量: {signal.get('quantity', 0)}\n"
                            f"价格: {signal.get('price', 0):.2f}\n"
                            f"原因: {signal.get('reason', '')}"
                        ),
                    }
                ],
            },
        }

    @staticmethod
    def format_risk_alert_card(alert: dict) -> dict:
        """风控告警卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {
                    "title": {"tag": "plain_text", "content": "🚨 风控告警"},
                    "template": "red",
                },
                "elements": [
                    {"tag": "markdown", "content": f"告警类型: {alert.get('type', '')}"},
                    {"tag": "markdown", "content": f"告警内容: {alert.get('message', '')}"},
                    {"tag": "markdown", "content": f"当前值: {alert.get('value', '')}"},
                ],
            },
        }

    @staticmethod
    def format_daily_report_card(report: dict) -> dict:
        """日报卡片."""
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": "📋 每日简报"}},
                "elements": [
                    {"tag": "markdown", "content": f"日期: {report.get('date', '')}"},
                    {"tag": "markdown", "content": f"组合收益: {report.get('return', 0):+.2%}"},
                    {"tag": "markdown", "content": f"基准收益: {report.get('benchmark_return', 0):+.2%}"},
                    {"tag": "markdown", "content": f"仓位: {report.get('position_pct', 0):.1%}"},
                ],
            },
        }

    @staticmethod
    def build_push_card(message) -> dict:
        """构建推送卡片."""
        template_map = {
            "trade_signal": MessageTemplates.format_trade_signal_card,
            "risk_alert": MessageTemplates.format_risk_alert_card,
            "daily_report": MessageTemplates.format_daily_report_card,
        }
        formatter = template_map.get(message.type.value)
        if formatter:
            return formatter(message.content)
        return {
            "msg_type": "interactive",
            "card": {
                "header": {"title": {"tag": "plain_text", "content": message.title}},
                "elements": [{"tag": "markdown", "content": str(message.content)}],
            },
        }
