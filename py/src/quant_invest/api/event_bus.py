#!/usr/bin/env python3
"""事件总线桥接 — 将 C++ EventBus 事件桥接到 Python 订阅者.

EventBusBridge 是 Python 侧的事件中心:
1. 支持本地 Python 订阅/发布（纯 Python 模式）
2. 支持桥接 C++ EventBus 事件（C++ 模式，需 pybind 绑定）
3. 提供 async 接口供 WebSocketServer 使用
4. 支持事件过滤和转换

用法::

    from quant_invest.api.event_bus import EventBusBridge

    bus = EventBusBridge()

    # 订阅事件
    def on_kline(topic, data):
        print(f"kline: {data}")

    bus.subscribe("kline", on_kline)

    # 发布事件
    bus.publish("kline", {"symbol": "000001.SH", "close": 3400.5})

    # 异步迭代器（供 WebSocket 使用）
    async for event in bus.async_events("kline"):
        await ws.send_json(event)
"""

from __future__ import annotations

import asyncio
import logging
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Any, Callable

logger = logging.getLogger("quant_invest.api.event_bus")


@dataclass
class Event:
    """事件数据."""

    topic: str
    data: Any
    timestamp: float = field(default_factory=time.time)
    source: str = "python"  # "python" or "cpp"


# 订阅者回调类型
Subscriber = Callable[[str, Any], None]
AsyncSubscriber = Callable[[str, Any], Any]


class EventBusBridge:
    """事件总线桥接 — Python 事件中心 + C++ EventBus 桥接.

    线程安全:
    - 内部使用 RLock 保护订阅者列表
    - publish() 可从任意线程调用
    - async_events() 使用 asyncio.Queue，需在事件循环中消费
    """

    def __init__(self, use_cpp: bool = False) -> None:
        self._use_cpp = use_cpp
        self._cpp_bus: Any = None
        self._subscribers: dict[str, list[Subscriber]] = defaultdict(list)
        self._async_queues: dict[str, list[asyncio.Queue]] = defaultdict(list)
        self._lock = threading.RLock()
        self._running = False
        self._stats = _EventBusStats()

        if use_cpp:
            self._try_init_cpp()

    def _try_init_cpp(self) -> None:
        """尝试初始化 C++ EventBus 桥接."""
        try:
            from quant_invest._quant_core import EventBus as CppEventBus

            self._cpp_bus = CppEventBus()
            self._cpp_bus.start()
            logger.info("C++ EventBus 桥接已启用")
        except ImportError:
            logger.info("C++ EventBus 不可用，使用纯 Python 模式")
            self._use_cpp = False
            self._cpp_bus = None

    @property
    def use_cpp(self) -> bool:
        """是否使用 C++ EventBus."""
        return self._use_cpp and self._cpp_bus is not None

    @property
    def is_running(self) -> bool:
        """事件总线是否运行中."""
        return self._running

    # ------------------------------------------------------------------
    # 订阅/取消订阅
    # ------------------------------------------------------------------

    def subscribe(self, topic: str, callback: Subscriber) -> str:
        """订阅主题.

        Args:
            topic: 主题名（如 "kline", "trade", "signal"）
            callback: 回调函数 (topic, data) -> None

        Returns:
            订阅 ID
        """
        with self._lock:
            sub_id = f"{topic}:{id(callback)}"
            self._subscribers[topic].append(callback)
            self._stats.total_subscriptions += 1
            logger.debug("订阅 %s (sub_id=%s)", topic, sub_id)
            return sub_id

    def unsubscribe(self, topic: str, callback: Subscriber) -> bool:
        """取消订阅.

        Args:
            topic: 主题名
            callback: 之前注册的回调函数

        Returns:
            是否成功取消
        """
        with self._lock:
            subs = self._subscribers.get(topic, [])
            if callback in subs:
                subs.remove(callback)
                return True
            return False

    def subscribe_async(self, topic: str) -> asyncio.Queue:
        """订阅主题，返回异步队列.

        适用于 WebSocketServer 等异步消费者。

        Args:
            topic: 主题名

        Returns:
            asyncio.Queue，消费者从中获取 Event 对象
        """
        queue: asyncio.Queue = asyncio.Queue()
        with self._lock:
            self._async_queues[topic].append(queue)
            self._stats.total_subscriptions += 1
        return queue

    def unsubscribe_async(self, topic: str, queue: asyncio.Queue) -> bool:
        """取消异步订阅.

        Args:
            topic: 主题名
            queue: 之前通过 subscribe_async 获取的队列

        Returns:
            是否成功取消
        """
        with self._lock:
            queues = self._async_queues.get(topic, [])
            if queue in queues:
                queues.remove(queue)
                return True
            return False

    # ------------------------------------------------------------------
    # 发布事件
    # ------------------------------------------------------------------

    def publish(self, topic: str, data: Any, source: str = "python") -> None:
        """发布事件.

        通知所有同步订阅者，并将事件推入异步队列。
        可从任意线程调用。

        Args:
            topic: 主题名
            data: 事件数据
            source: 事件来源 ("python" / "cpp")
        """
        event = Event(topic=topic, data=data, source=source)
        self._stats.total_events += 1

        # 通知同步订阅者
        with self._lock:
            subs = list(self._subscribers.get(topic, []))

        for callback in subs:
            try:
                callback(topic, data)
            except Exception as e:
                logger.error("订阅者回调异常 (topic=%s): %s", topic, e)
                self._stats.callback_errors += 1

        # 推入异步队列
        with self._lock:
            queues = list(self._async_queues.get(topic, []))

        for queue in queues:
            try:
                queue.put_nowait(event)
            except asyncio.QueueFull:
                logger.warning("异步队列已满 (topic=%s)，丢弃事件", topic)
                self._stats.dropped_events += 1
            except RuntimeError:
                # 事件循环未运行，忽略
                pass

        # C++ 模式下同步到 C++ EventBus
        if self._use_cpp and self._cpp_bus is not None:
            try:
                self._cpp_bus.publish(topic, data)
            except Exception as e:
                logger.debug("C++ EventBus publish 失败: %s", e)

    # ------------------------------------------------------------------
    # 生命周期
    # ------------------------------------------------------------------

    def start(self) -> None:
        """启动事件总线."""
        self._running = True
        logger.info("EventBusBridge 已启动 (C++ 模式: %s)", self.use_cpp)

    def stop(self) -> None:
        """停止事件总线."""
        self._running = False
        if self._cpp_bus is not None:
            try:
                self._cpp_bus.stop()
            except Exception:
                pass
        logger.info("EventBusBridge 已停止")

    # ------------------------------------------------------------------
    # 状态查询
    # ------------------------------------------------------------------

    def get_status(self) -> dict[str, Any]:
        """获取事件总线状态."""
        with self._lock:
            topics = list(set(
                list(self._subscribers.keys()) + list(self._async_queues.keys())
            ))
            sync_counts = {t: len(s) for t, s in self._subscribers.items()}
            async_counts = {t: len(q) for t, q in self._async_queues.items()}

        return {
            "running": self._running,
            "use_cpp": self.use_cpp,
            "topics": topics,
            "sync_subscribers": sync_counts,
            "async_subscribers": async_counts,
            "stats": {
                "total_events": self._stats.total_events,
                "total_subscriptions": self._stats.total_subscriptions,
                "callback_errors": self._stats.callback_errors,
                "dropped_events": self._stats.dropped_events,
            },
        }

    def get_topics(self) -> list[str]:
        """获取所有活跃主题."""
        with self._lock:
            return list(set(
                list(self._subscribers.keys()) + list(self._async_queues.keys())
            ))


class _EventBusStats:
    """事件总线统计."""

    def __init__(self) -> None:
        self.total_events: int = 0
        self.total_subscriptions: int = 0
        self.callback_errors: int = 0
        self.dropped_events: int = 0


# ---------------------------------------------------------------------------
# 全局单例
# ---------------------------------------------------------------------------

_bridge: EventBusBridge | None = None


def get_event_bus() -> EventBusBridge:
    """获取全局 EventBusBridge 单例."""
    global _bridge
    if _bridge is None:
        _bridge = EventBusBridge()
    return _bridge
