#!/usr/bin/env python3
"""执行引擎模块 — 封装 C++ OrderManager + MockBroker 的订单模拟接口."""

from .order_adapter import OrderAdapter, OrderInfo, OrderResult

__all__ = [
    "OrderAdapter",
    "OrderInfo",
    "OrderResult",
]
