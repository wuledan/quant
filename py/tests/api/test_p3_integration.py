#!/usr/bin/env python3
"""P3 集成测试 — 验证 WebSocketServer + EventBusBridge.

测试场景:
1. EventBusBridge 同步订阅/发布
2. EventBusBridge 异步订阅
3. WebSocketServer 初始化和状态
4. EventBusBridge + DataSchedulerService 集成
5. 事件广播和频道过滤
"""

import asyncio
import sys
import time
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "py" / "src"))


class TestP3Integration:
    """P3 集成测试."""

    def test_event_bus_sync_subscribe(self) -> None:
        """测试 EventBusBridge 同步订阅/发布."""
        from quant_invest.api.event_bus import EventBusBridge

        bus = EventBusBridge()
        bus.start()

        received = []

        def on_kline(topic, data):
            received.append((topic, data))

        bus.subscribe("kline", on_kline)
        bus.publish("kline", {"symbol": "000001.SH", "close": 3400.5})
        bus.publish("kline", {"symbol": "600519.SH", "close": 1800.0})

        assert len(received) == 2
        assert received[0][0] == "kline"
        assert received[0][1]["symbol"] == "000001.SH"
        assert received[1][1]["symbol"] == "600519.SH"

        bus.stop()

    def test_event_bus_unsubscribe(self) -> None:
        """测试 EventBusBridge 取消订阅."""
        from quant_invest.api.event_bus import EventBusBridge

        bus = EventBusBridge()
        bus.start()

        received = []

        def on_signal(topic, data):
            received.append(data)

        bus.subscribe("signal", on_signal)
        bus.publish("signal", {"action": "buy"})
        assert len(received) == 1

        bus.unsubscribe("signal", on_signal)
        bus.publish("signal", {"action": "sell"})
        assert len(received) == 1  # 取消后不应收到

        bus.stop()

    def test_event_bus_async_subscribe(self) -> None:
        """测试 EventBusBridge 异步订阅."""
        from quant_invest.api.event_bus import EventBusBridge

        async def _test():
            bus = EventBusBridge()
            bus.start()

            queue = bus.subscribe_async("kline")

            bus.publish("kline", {"symbol": "000001.SH"})
            bus.publish("kline", {"symbol": "600519.SH"})

            # 消费异步队列
            event1 = await asyncio.wait_for(queue.get(), timeout=1.0)
            assert event1.topic == "kline"
            assert event1.data["symbol"] == "000001.SH"

            event2 = await asyncio.wait_for(queue.get(), timeout=1.0)
            assert event2.data["symbol"] == "600519.SH"

            bus.stop()

        asyncio.run(_test())

    def test_event_bus_multiple_topics(self) -> None:
        """测试 EventBusBridge 多主题."""
        from quant_invest.api.event_bus import EventBusBridge

        bus = EventBusBridge()
        bus.start()

        kline_received = []
        signal_received = []

        bus.subscribe("kline", lambda t, d: kline_received.append(d))
        bus.subscribe("signal", lambda t, d: signal_received.append(d))

        bus.publish("kline", {"close": 3400.5})
        bus.publish("signal", {"action": "buy"})
        bus.publish("kline", {"close": 3405.0})

        assert len(kline_received) == 2
        assert len(signal_received) == 1

        bus.stop()

    def test_event_bus_status(self) -> None:
        """测试 EventBusBridge 状态查询."""
        from quant_invest.api.event_bus import EventBusBridge

        bus = EventBusBridge()
        bus.start()

        bus.subscribe("kline", lambda t, d: None)
        bus.publish("kline", {"test": True})

        status = bus.get_status()
        assert status["running"] is True
        assert "kline" in status["topics"]
        assert status["stats"]["total_events"] >= 1
        assert status["stats"]["total_subscriptions"] >= 1

        bus.stop()

    def test_ws_server_init(self) -> None:
        """测试 WebSocketServer 初始化."""
        from quant_invest.api.ws_server import WebSocketServer
        from quant_invest.api.event_bus import EventBusBridge

        bus = EventBusBridge()
        ws = WebSocketServer(event_bus=bus)

        assert ws.connection_count == 0
        status = ws.get_status()
        assert "valid_channels" in status
        assert "kline" in status["valid_channels"]

    def test_event_bus_with_scheduler(self) -> None:
        """测试 EventBusBridge 与 DataSchedulerService 集成."""
        from quant_invest.api.event_bus import EventBusBridge
        from quant_invest.data.scheduler import DataSchedulerService

        bus = EventBusBridge()
        bus.start()

        received = []
        bus.subscribe("kline", lambda t, d: received.append(d))

        # 模拟调度器发布事件
        svc = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
        )

        # 获取数据后发布事件
        df = svc.get_daily_data("000001.SH")
        if not df.empty:
            bus.publish("kline", {
                "symbol": "000001.SH",
                "close": float(df["close"].iloc[-1]),
                "volume": int(df["volume"].iloc[-1]),
            })

        assert len(received) == 1
        assert received[0]["symbol"] == "000001.SH"

        bus.stop()

    def test_event_bus_global_singleton(self) -> None:
        """测试全局 EventBusBridge 单例."""
        from quant_invest.api.event_bus import get_event_bus

        bus1 = get_event_bus()
        bus2 = get_event_bus()
        assert bus1 is bus2, "应返回同一个实例"
