#!/usr/bin/env python3
"""信号推送服务."""

from __future__ import annotations

from dataclasses import dataclass
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
    timestamp: datetime = None  # type: ignore[assignment]
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
        # TODO: 实现飞书API调用
        return {"success": True}
