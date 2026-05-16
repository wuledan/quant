#!/usr/bin/env python3
"""信号推送服务."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum


class PushType(str, Enum):
    """推送消息类型."""

    TRADE_SIGNAL = "trade_signal"
    RISK_ALERT = "risk_alert"
    DAILY_REPORT = "daily_report"
    BACKTEST_DONE = "backtest_done"
    MODEL_TRAIN_DONE = "model_done"


@dataclass
class PushMessage:
    """推送消息."""

    type: PushType
    title: str
    content: dict
    timestamp: datetime | None = None
    priority: str = "normal"


class SignalPusher:
    """信号推送服务."""

    def __init__(self, webhook_url: str = "") -> None:
        self.webhook_url = webhook_url

    async def push_trade_signal(self, signal: dict) -> dict:
        """推送交易信号."""
        return await self._send(
            PushMessage(
                type=PushType.TRADE_SIGNAL, title="交易信号", content=signal, priority="high"
            )
        )

    async def push_risk_alert(self, alert: dict) -> dict:
        """推送风控告警."""
        return await self._send(
            PushMessage(
                type=PushType.RISK_ALERT, title="风控告警", content=alert, priority="urgent"
            )
        )

    async def push_daily_report(self, report: dict) -> dict:
        """推送日报."""
        return await self._send(
            PushMessage(type=PushType.DAILY_REPORT, title="日报", content=report, priority="normal")
        )

    async def _send(self, message: PushMessage) -> dict:
        """发送消息到飞书."""
        if not self.webhook_url:
            return {"success": False, "error": "webhook_url not configured"}

        from quant_invest.bot.templates import MessageTemplates

        card = MessageTemplates.build_push_card(message)

        try:
            import httpx

            async with httpx.AsyncClient(timeout=10.0) as client:
                resp = await client.post(self.webhook_url, json=card)
                return {"success": resp.is_success, "status_code": resp.status_code}
        except Exception as e:
            return {"success": False, "error": str(e)}
