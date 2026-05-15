"""WebSocket连接管理."""

from __future__ import annotations

from fastapi import WebSocket


class WebSocketManager:
    """WebSocket连接管理器."""

    def __init__(self) -> None:
        self._connections: dict[str, set[WebSocket]] = {}

    async def connect(self, channel: str, websocket: WebSocket) -> None:
        """建立WebSocket连接."""
        await websocket.accept()
        if channel not in self._connections:
            self._connections[channel] = set()
        self._connections[channel].add(websocket)

    async def disconnect(self, channel: str, websocket: WebSocket) -> None:
        """断开连接."""
        self._connections[channel].discard(websocket)

    async def broadcast(self, channel: str, message: dict) -> None:
        """广播消息."""
        if channel not in self._connections:
            return
        disconnected = set()
        for ws in self._connections[channel]:
            try:
                await ws.send_json(message)
            except Exception:
                disconnected.add(ws)
        self._connections[channel] -= disconnected

    async def close(self) -> None:
        """关闭所有连接."""
        for channel in self._connections:
            for ws in self._connections[channel]:
                await ws.close()
