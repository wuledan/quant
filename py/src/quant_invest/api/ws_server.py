#!/usr/bin/env python3
"""WebSocket 实时推送服务 — 将 EventBus 事件推送给前端.

WebSocketServer 在 FastAPI 应用启动时初始化:
1. 为每个 WebSocket 连接创建异步订阅
2. 从 EventBusBridge.async_events() 消费事件
3. 将事件序列化为 JSON 推送给客户端
4. 支持频道过滤和心跳保活

支持频道:
- kline: K 线数据更新
- trade: 成交推送
- signal: 策略信号
- risk: 风控告警
- portfolio: 持仓变动
- system: 系统状态
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any

from fastapi import WebSocket

from .event_bus import EventBusBridge, Event, get_event_bus

logger = logging.getLogger("quant_invest.api.ws_server")

# 心跳间隔（秒）
HEARTBEAT_INTERVAL = 30

# 支持的频道
VALID_CHANNELS = {"kline", "trade", "signal", "risk", "portfolio", "system", "backtest"}


@dataclass
class WSConnection:
    """WebSocket 连接信息."""

    websocket: WebSocket
    channels: set[str] = field(default_factory=set)
    connected_at: float = field(default_factory=time.time)
    last_heartbeat: float = field(default_factory=time.time)
    messages_sent: int = 0


class WebSocketServer:
    """WebSocket 实时推送服务.

    管理所有 WebSocket 连接，将 EventBus 事件推送给订阅了相应频道的客户端。
    """

    def __init__(self, event_bus: EventBusBridge | None = None) -> None:
        self._event_bus = event_bus or get_event_bus()
        self._connections: dict[int, WSConnection] = {}
        self._lock = asyncio.Lock()
        self._running = False
        self._stats = _WSServerStats()

    @property
    def event_bus(self) -> EventBusBridge:
        """关联的事件总线."""
        return self._event_bus

    @property
    def connection_count(self) -> int:
        """当前连接数."""
        return len(self._connections)

    # ------------------------------------------------------------------
    # 连接管理
    # ------------------------------------------------------------------

    async def handle_connection(self, websocket: WebSocket) -> None:
        """处理 WebSocket 连接生命周期.

        1. 接受连接
        2. 进入消息循环（处理订阅/心跳）
        3. 同时推送 EventBus 事件
        4. 连接断开时清理

        Args:
            websocket: FastAPI WebSocket 对象
        """
        await websocket.accept()
        conn_id = id(websocket)
        conn = WSConnection(websocket=websocket)

        async with self._lock:
            self._connections[conn_id] = conn
            self._stats.total_connections += 1

        logger.info("WebSocket 连接: conn_id=%d, 当前连接数=%d", conn_id, len(self._connections))

        # 为每个已订阅频道创建事件推送任务
        push_tasks: dict[str, asyncio.Task] = {}

        try:
            while True:
                data = await websocket.receive_json()
                action = data.get("action")

                if action == "subscribe":
                    channels = data.get("channels", [])
                    await self._handle_subscribe(conn, channels, push_tasks)

                elif action == "unsubscribe":
                    channels = data.get("channels", [])
                    await self._handle_unsubscribe(conn, channels, push_tasks)

                elif action == "ping":
                    await websocket.send_json({"type": "pong", "ts": time.time()})

        except Exception:
            pass
        finally:
            # 清理
            async with self._lock:
                self._connections.pop(conn_id, None)

            # 取消所有推送任务
            for task in push_tasks.values():
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

            # 取消异步订阅
            for channel in conn.channels:
                pass  # Queue 会自动 GC

            logger.info("WebSocket 断开: conn_id=%d", conn_id)

    async def _handle_subscribe(
        self,
        conn: WSConnection,
        channels: list[str],
        push_tasks: dict[str, asyncio.Task],
    ) -> None:
        """处理订阅请求."""
        valid = [ch for ch in channels if ch in VALID_CHANNELS]
        if not valid:
            await conn.websocket.send_json({
                "type": "error",
                "message": f"无效频道: {channels}, 有效频道: {VALID_CHANNELS}",
            })
            return

        for channel in valid:
            if channel in conn.channels:
                continue

            conn.channels.add(channel)

            # 创建异步订阅和推送任务
            queue = self._event_bus.subscribe_async(channel)
            task = asyncio.create_task(
                self._push_events(conn, channel, queue),
                name=f"ws_push_{channel}_{id(conn.websocket)}",
            )
            push_tasks[channel] = task

        await conn.websocket.send_json({
            "type": "subscribed",
            "channels": valid,
        })

        logger.debug("WebSocket 订阅: conn_id=%d, channels=%s", id(conn.websocket), valid)

    async def _handle_unsubscribe(
        self,
        conn: WSConnection,
        channels: list[str],
        push_tasks: dict[str, asyncio.Task],
    ) -> None:
        """处理取消订阅请求."""
        for channel in channels:
            conn.channels.discard(channel)
            task = push_tasks.pop(channel, None)
            if task:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass

        await conn.websocket.send_json({
            "type": "unsubscribed",
            "channels": channels,
        })

    async def _push_events(
        self,
        conn: WSConnection,
        channel: str,
        queue: asyncio.Queue,
    ) -> None:
        """从异步队列消费事件并推送给客户端.

        Args:
            conn: WebSocket 连接
            channel: 频道名
            queue: 异步事件队列
        """
        try:
            while True:
                event: Event = await queue.get()
                try:
                    message = {
                        "type": "event",
                        "channel": channel,
                        "topic": event.topic,
                        "data": event.data,
                        "ts": event.timestamp,
                        "source": event.source,
                    }
                    await conn.websocket.send_json(message)
                    conn.messages_sent += 1
                except Exception as e:
                    logger.debug("推送事件失败: %s", e)
                    break
        except asyncio.CancelledError:
            pass

    # ------------------------------------------------------------------
    # 广播（服务端主动推送）
    # ------------------------------------------------------------------

    async def broadcast(self, channel: str, data: Any) -> None:
        """向所有订阅了指定频道的客户端广播消息.

        Args:
            channel: 频道名
            data: 消息数据
        """
        self._event_bus.publish(channel, data)

    # ------------------------------------------------------------------
    # 状态
    # ------------------------------------------------------------------

    def get_status(self) -> dict[str, Any]:
        """获取 WebSocket 服务状态."""
        return {
            "running": self._running,
            "connections": len(self._connections),
            "valid_channels": list(VALID_CHANNELS),
            "stats": {
                "total_connections": self._stats.total_connections,
            },
        }


class _WSServerStats:
    """WebSocket 服务统计."""

    def __init__(self) -> None:
        self.total_connections: int = 0


# ---------------------------------------------------------------------------
# 全局单例
# ---------------------------------------------------------------------------

_server: WebSocketServer | None = None


def get_ws_server() -> WebSocketServer:
    """获取全局 WebSocketServer 单例."""
    global _server
    if _server is None:
        _server = WebSocketServer()
    return _server
