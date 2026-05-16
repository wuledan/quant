#!/usr/bin/env python3
"""飞书机器人模块测试."""

from __future__ import annotations

import json
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from quant_invest.bot import (
    BacktestCommand,
    CommandParser,
    FactorCommand,
    FeishuBot,
    HelpCommand,
    MessageTemplates,
    PerformanceCommand,
    PortfolioCommand,
    PushMessage,
    PushType,
    SignalPusher,
    StrategyCommand,
)


class TestFeishuBot:
    """FeishuBot核心测试."""

    @pytest.fixture
    def bot(self) -> FeishuBot:
        return FeishuBot(
            app_id="test_app_id",
            app_secret="test_secret",
            verification_token="test_token",
        )

    @pytest.mark.asyncio
    async def test_start_stop(self, bot: FeishuBot):
        """启动/停止生命周期."""
        assert not bot.is_running
        await bot.start()
        assert bot.is_running
        await bot.stop()
        assert not bot.is_running

    @pytest.mark.asyncio
    async def test_url_verification(self, bot: FeishuBot):
        """URL验证挑战."""
        result = await bot.handle_event(
            {"type": "url_verification", "challenge": "test_challenge"}
        )
        assert result["challenge"] == "test_challenge"

    @pytest.mark.asyncio
    async def test_text_message_no_handler(self, bot: FeishuBot):
        """未注册处理器的文本消息."""
        event = {
            "event": {
                "msg_type": "text",
                "message": {"content": json.dumps({"text": "/unknown_command"})},
            }
        }
        result = await bot.handle_event(event)
        assert not result.get("success", True)

    @pytest.mark.asyncio
    async def test_unknown_event_type(self, bot: FeishuBot):
        """未知事件类型."""
        result = await bot.handle_event({"event": {"msg_type": "unknown"}})
        assert result.get("success")

    def test_register_handler(self, bot: FeishuBot):
        """注册指令处理器."""
        handler = PortfolioCommand()
        bot.register_handler("测试", handler)
        keyword, _args = bot.parser.parse("/测试 arg1")
        assert keyword == "测试"

    def test_verify_signature_no_token(self):
        """无验证token时签名默认通过."""
        bot = FeishuBot(app_id="", app_secret="", verification_token="")
        assert bot.verify_signature(b"body", "sig")


class TestCommandParser:
    """指令解析器测试."""

    @pytest.fixture
    def parser(self) -> CommandParser:
        p = CommandParser()
        p.register("持仓", PortfolioCommand())
        p.register("收益", PerformanceCommand())
        p.register("帮助", HelpCommand())
        return p

    def test_parse_with_slash(self, parser: CommandParser):
        """带斜杠解析."""
        keyword, args = parser.parse("/持仓")
        assert keyword == "持仓"
        assert args == []

    def test_parse_with_args(self, parser: CommandParser):
        """带参数解析."""
        keyword, args = parser.parse("/持仓 000001.SZ")
        assert keyword == "持仓"
        assert args == ["000001.SZ"]

    def test_parse_without_slash(self, parser: CommandParser):
        """无斜杠解析."""
        keyword, args = parser.parse("收益 本月")
        assert keyword == "收益"
        assert args == ["本月"]

    def test_parse_empty(self, parser: CommandParser):
        """空文本."""
        keyword, args = parser.parse("")
        assert keyword == ""
        assert args == []

    @pytest.mark.asyncio
    async def test_dispatch_known(self, parser: CommandParser):
        """已知指令分发."""
        result = await parser.dispatch("/帮助", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_dispatch_unknown_fallsback_to_help(self, parser: CommandParser):
        """未知指令回退到帮助."""
        result = await parser.dispatch("/unknown", {})
        assert result.get("success")


class TestCommandHandlers:
    """指令处理器测试."""

    @pytest.mark.asyncio
    async def test_portfolio_command(self):
        """持仓指令."""
        handler = PortfolioCommand()
        result = await handler.handle("/持仓", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_performance_command(self):
        """收益指令."""
        handler = PerformanceCommand()
        result = await handler.handle("/收益", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_backtest_command(self):
        """回测指令."""
        handler = BacktestCommand()
        result = await handler.handle("/回测", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_backtest_run_command(self):
        """回测运行指令."""
        handler = BacktestCommand()
        result = await handler.handle("/回测 运行 策略A 2024-01-01 2024-12-31", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_factor_command(self):
        """因子指令."""
        handler = FactorCommand()
        result = await handler.handle("/因子", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_factor_query(self):
        """因子查询指令."""
        handler = FactorCommand()
        result = await handler.handle("/因子 动量20", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_strategy_command(self):
        """策略指令."""
        handler = StrategyCommand()
        result = await handler.handle("/策略", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_strategy_start(self):
        """启动策略."""
        handler = StrategyCommand()
        result = await handler.handle("/策略 启动 动量策略", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_strategy_stop(self):
        """停止策略."""
        handler = StrategyCommand()
        result = await handler.handle("/策略 停止 动量策略", {})
        assert result.get("success")

    @pytest.mark.asyncio
    async def test_help_command(self):
        """帮助指令."""
        handler = HelpCommand()
        result = await handler.handle("/帮助", {})
        assert result.get("success")
        assert "可用指令" in result.get("message", "")

    @pytest.mark.asyncio
    async def test_help_with_topic(self):
        """带话题的帮助."""
        handler = HelpCommand()
        result = await handler.handle("/帮助 持仓", {})
        assert result.get("success")


class TestMessageTemplates:
    """消息模板测试."""

    def test_portfolio_card(self):
        """持仓卡片."""
        positions = [
            {"symbol": "000001.SZ", "name": "平安银行", "pnl_pct": 5.2},
            {"symbol": "600519.SH", "name": "贵州茅台", "pnl_pct": -1.5},
        ]
        card = MessageTemplates.format_portfolio_card(positions)
        assert card["msg_type"] == "interactive"
        assert "持仓" in card["card"]["header"]["title"]["content"]
        assert len(card["card"]["elements"]) == 2

    def test_performance_card(self):
        """绩效卡片."""
        card = MessageTemplates.format_performance_card(
            {"total_return": 0.15, "sharpe": 1.2, "max_drawdown": -0.08}
        )
        assert card["msg_type"] == "interactive"

    def test_trade_signal_card(self):
        """交易信号卡片."""
        card = MessageTemplates.format_trade_signal_card(
            {"symbol": "000001.SZ", "direction": "BUY", "quantity": 1000, "price": 10.5}
        )
        assert "000001.SZ" in str(card["card"]["elements"])

    def test_risk_alert_card(self):
        """风控告警卡片."""
        card = MessageTemplates.format_risk_alert_card(
            {"type": "max_drawdown", "message": "回撤超限", "value": "-15%"}
        )
        assert "风控告警" in card["card"]["header"]["title"]["content"]

    def test_daily_report_card(self):
        """日报卡片."""
        card = MessageTemplates.format_daily_report_card(
            {"date": "2024-01-02", "return": 0.01, "benchmark_return": 0.005, "position_pct": 0.8}
        )
        assert "每日简报" in card["card"]["header"]["title"]["content"]

    def test_portfolio_summary_card(self):
        """持仓汇总卡片."""
        card = MessageTemplates.format_portfolio_summary_card(
            {"total_value": 1_200_000, "position_count": 5, "daily_pnl": 15000}
        )
        assert "持仓汇总" in card["card"]["header"]["title"]["content"]

    def test_build_push_card_known_type(self):
        """已知类型的推送卡片."""
        msg = PushMessage(
            type=PushType.TRADE_SIGNAL,
            title="交易信号",
            content={"symbol": "000001.SZ", "direction": "BUY"},
        )
        card = MessageTemplates.build_push_card(msg)
        assert "交易信号" in card["card"]["header"]["title"]["content"]


class TestSignalPusher:
    """信号推送测试."""

    @pytest.mark.asyncio
    async def test_push_no_webhook(self):
        """无webhook配置."""
        pusher = SignalPusher()
        result = await pusher.push_trade_signal({})
        assert not result["success"]
        assert "not configured" in result["error"]

    @pytest.mark.asyncio
    async def test_trade_signal(self):
        """推送交易信号."""
        pusher = SignalPusher(webhook_url="http://test.webhook")
        with patch("httpx.AsyncClient") as mock_client:
            mock_instance = AsyncMock()
            mock_instance.post.return_value = MagicMock(is_success=True, status_code=200)
            mock_client.return_value.__aenter__.return_value = mock_instance
            mock_client.return_value.__aenter__.return_value.post.return_value = MagicMock(
                is_success=True, status_code=200
            )

            result = await pusher.push_trade_signal(
                {"symbol": "000001.SZ", "direction": "BUY"}
            )
            assert result.get("success")

    @pytest.mark.asyncio
    async def test_risk_alert(self):
        """推送风控告警."""
        pusher = SignalPusher(webhook_url="http://test.webhook")
        with patch("httpx.AsyncClient") as mock_client:
            mock_client.return_value.__aenter__.return_value.post.return_value = MagicMock(
                is_success=True, status_code=200
            )
            result = await pusher.push_risk_alert({"type": "max_drawdown", "message": "test"})
            assert result.get("success")


class TestPushMessage:
    """推送消息数据类测试."""

    def test_push_message_defaults(self):
        """默认值."""
        msg = PushMessage(type=PushType.DAILY_REPORT, title="日报", content={"key": "value"})
        assert msg.priority == "normal"
        assert msg.timestamp is None

    def test_push_message_custom(self):
        """自定义值."""
        from datetime import datetime

        ts = datetime.now()
        msg = PushMessage(
            type=PushType.RISK_ALERT,
            title="告警",
            content={"level": "high"},
            timestamp=ts,
            priority="urgent",
        )
        assert msg.priority == "urgent"
        assert msg.timestamp == ts

    def test_push_type_enum(self):
        """枚举值."""
        assert PushType.TRADE_SIGNAL.value == "trade_signal"
        assert PushType.RISK_ALERT.value == "risk_alert"
        assert PushType.DAILY_REPORT.value == "daily_report"
