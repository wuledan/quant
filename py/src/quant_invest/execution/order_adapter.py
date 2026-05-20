#!/usr/bin/env python3
"""订单适配器 — 封装 C++ OrderManager 和 MockBroker，为回测引擎提供订单模拟接口.

将 C++ 执行引擎的 OrderManager + MockBroker 封装为 Python 友好的接口，
支持订单提交、取消、查询，以及模拟成交。

当 C++ _quant_core 模块不可用时，自动回退到纯 Python 模拟。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any

logger = logging.getLogger("quant_invest.execution.order_adapter")


# ---------------------------------------------------------------------------
# C++ Result<void> 调用辅助
# ---------------------------------------------------------------------------

def _cpp_call_void(func, *args) -> None:
    """调用返回 Result<void> 的 C++ 方法.

    pybind11 尚未注册 Result<void> 的 Python 转换器，
    导致返回值转换时抛出 TypeError。但 C++ 侧的状态变更
    在异常抛出前已完成，因此捕获 TypeError 即可。

    Args:
        func: C++ 方法（如 om.on_order_accepted）
        *args: 方法参数
    """
    try:
        func(*args)
    except TypeError as e:
        # Result<void> 无法转换为 Python 类型，但 C++ 侧已执行
        if "Result<void>" in str(e) or "Unable to convert" in str(e):
            logger.debug("C++ Result<void> 返回值转换忽略: %s", e)
        else:
            raise


# ---------------------------------------------------------------------------
# 数据结构定义
# ---------------------------------------------------------------------------

@dataclass
class OrderResult:
    """订单提交结果."""

    order_id: int | None = None
    status: str = ""
    filled_qty: int = 0
    fill_price: float = 0.0
    reject_reason: str = ""

    @property
    def success(self) -> bool:
        """订单是否成功提交."""
        return self.order_id is not None and self.status not in ("REJECTED", "")


@dataclass
class OrderInfo:
    """订单信息（从 C++ Order 转换）."""

    order_id: int = 0
    symbol: str = ""
    side: str = ""
    order_type: str = ""
    status: str = ""
    price: float = 0.0
    quantity: int = 0
    filled_quantity: int = 0
    avg_fill_price: float = 0.0
    reject_reason: str = ""


# ---------------------------------------------------------------------------
# C++ 枚举映射
# ---------------------------------------------------------------------------

# 买卖方向映射
_SIDE_MAP = {"BUY": "BUY", "SELL": "SELL"}
_SIDE_CPP_MAP = {}  # 运行时填充

# 订单类型映射
_ORDER_TYPE_MAP = {"MARKET": "MARKET", "LIMIT": "LIMIT", "STOP": "STOP"}
_ORDER_TYPE_CPP_MAP = {}  # 运行时填充

# 订单状态映射（C++ enum → Python 字符串）
_STATUS_CPP_TO_STR = {}  # 运行时填充

# 有效期映射
_TIF_CPP_MAP = {}  # 运行时填充


def _build_enum_maps() -> None:
    """构建 C++ 枚举 → Python 字符串的映射表."""
    global _SIDE_CPP_MAP, _ORDER_TYPE_CPP_MAP, _STATUS_CPP_TO_STR, _TIF_CPP_MAP

    try:
        from quant_invest._quant_core import (
            OrderSide,
            OrderType,
            OrderStatus,
            TimeInForce,
        )

        _SIDE_CPP_MAP = {
            "BUY": OrderSide.BUY,
            "SELL": OrderSide.SELL,
        }
        _ORDER_TYPE_CPP_MAP = {
            "MARKET": OrderType.MARKET,
            "LIMIT": OrderType.LIMIT,
            "STOP": OrderType.STOP,
            "STOP_LIMIT": OrderType.STOP_LIMIT,
        }
        _STATUS_CPP_TO_STR = {
            OrderStatus.PENDING_NEW: "PENDING_NEW",
            OrderStatus.NEW: "NEW",
            OrderStatus.PARTIAL_FILLED: "PARTIAL_FILLED",
            OrderStatus.FILLED: "FILLED",
            OrderStatus.CANCELLED: "CANCELLED",
            OrderStatus.PENDING_CANCEL: "PENDING_CANCEL",
            OrderStatus.REJECTED: "REJECTED",
            OrderStatus.EXPIRED: "EXPIRED",
            OrderStatus.SUSPENDED: "SUSPENDED",
        }
        _TIF_CPP_MAP = {
            "DAY": TimeInForce.DAY,
            "IOC": TimeInForce.IOC,
            "GTC": TimeInForce.GTC,
        }
    except ImportError:
        logger.info("C++ 执行引擎不可用，枚举映射表为空")


# ---------------------------------------------------------------------------
# OrderAdapter — 核心适配器
# ---------------------------------------------------------------------------

class OrderAdapter:
    """订单适配器 — 封装 C++ OrderManager + MockBroker.

    提供统一的订单模拟接口:
    - submit_order(): 提交订单，返回 OrderResult
    - cancel_order(): 取消订单
    - get_order(): 查询订单状态
    - get_active_orders(): 获取活跃订单列表
    - connect_broker(): 连接 MockBroker
    - disconnect_broker(): 断开 MockBroker

    当 C++ _quant_core 模块不可用时，回退到纯 Python 模拟。

    用法::

        adapter = OrderAdapter()
        adapter.connect_broker()
        result = adapter.submit_order(
            symbol="000001.SZ",
            side="BUY",
            order_type="MARKET",
            price=10.0,
            quantity=1000,
        )
        print(result)  # OrderResult(order_id=1, status="FILLED", ...)
    """

    def __init__(self) -> None:
        self._cpp_available = False
        self._order_manager: Any = None
        self._broker: Any = None
        self._connected = False

        # 纯 Python 回退状态
        self._python_orders: dict[int, dict] = {}
        self._python_next_id: int = 1

        # 尝试初始化 C++ 执行引擎
        self._try_init_cpp()

    def _try_init_cpp(self) -> None:
        """尝试初始化 C++ OrderManager 和 MockBroker."""
        try:
            from quant_invest._quant_core import (
                OrderManager,
                MockBroker,
                BrokerConfig,
            )

            _build_enum_maps()

            self._order_manager = OrderManager()
            self._broker = MockBroker(BrokerConfig())
            self._cpp_available = True
            logger.info("C++ 执行引擎初始化成功")
        except ImportError as e:
            self._cpp_available = False
            logger.info("C++ 执行引擎不可用，使用纯 Python 回退: %s", e)

    @property
    def cpp_available(self) -> bool:
        """C++ 执行引擎是否可用."""
        return self._cpp_available

    @property
    def is_connected(self) -> bool:
        """MockBroker 是否已连接."""
        return self._connected

    # ------------------------------------------------------------------
    # 连接管理
    # ------------------------------------------------------------------

    def connect_broker(self) -> None:
        """连接 MockBroker.

        在提交订单前必须先连接。C++ 模式下调用 MockBroker.connect()，
        Python 回退模式下直接标记为已连接。
        """
        if self._cpp_available and self._broker is not None:
            self._broker.connect()
            # 验证连接状态
            from quant_invest._quant_core import ConnectionStatus
            if self._broker.status == ConnectionStatus.CONNECTED:
                self._connected = True
                logger.info("MockBroker 已连接")
            else:
                logger.warning("MockBroker 连接失败，状态: %s", self._broker.status)
        else:
            self._connected = True
            logger.info("纯 Python 模拟经纪商已就绪")

    def disconnect_broker(self) -> None:
        """断开 MockBroker."""
        if self._cpp_available and self._broker is not None:
            self._broker.disconnect()
            self._connected = False
            logger.info("MockBroker 已断开")
        else:
            self._connected = False
            logger.info("纯 Python 模拟经纪商已断开")

    # ------------------------------------------------------------------
    # 订单操作
    # ------------------------------------------------------------------

    def submit_order(
        self,
        symbol: str,
        side: str = "BUY",
        order_type: str = "MARKET",
        price: float = 0.0,
        quantity: int = 0,
        stop_price: float = 0.0,
        time_in_force: str = "DAY",
    ) -> OrderResult:
        """提交订单.

        Args:
            symbol: 标的代码（如 "000001.SZ"）
            side: 买卖方向 "BUY" / "SELL"
            order_type: 订单类型 "MARKET" / "LIMIT" / "STOP"
            price: 订单价格（MARKET 单可设 0）
            quantity: 订单数量
            stop_price: 止损价（STOP 单使用）
            time_in_force: 有效期 "DAY" / "IOC" / "GTC"

        Returns:
            OrderResult 包含 order_id, status, filled_qty, fill_price
        """
        if not self._connected:
            logger.warning("MockBroker 未连接，订单提交失败")
            return OrderResult(status="REJECTED", reject_reason="Broker not connected")

        if self._cpp_available:
            return self._submit_order_cpp(
                symbol, side, order_type, price, quantity,
                stop_price, time_in_force,
            )
        else:
            return self._submit_order_python(
                symbol, side, order_type, price, quantity,
            )

    def cancel_order(self, order_id: int) -> bool:
        """取消订单.

        Args:
            order_id: 订单 ID

        Returns:
            是否成功取消
        """
        if self._cpp_available:
            return self._order_manager.cancel_order(order_id)
        else:
            order = self._python_orders.get(order_id)
            if order is None:
                return False
            if order["status"] in ("PENDING_NEW", "NEW"):
                order["status"] = "CANCELLED"
                return True
            return False

    def get_order(self, order_id: int) -> OrderInfo | None:
        """查询订单状态.

        Args:
            order_id: 订单 ID

        Returns:
            OrderInfo 或 None（订单不存在）
        """
        if self._cpp_available:
            return self._get_order_cpp(order_id)
        else:
            order = self._python_orders.get(order_id)
            if order is None:
                return None
            return OrderInfo(
                order_id=order_id,
                symbol=order["symbol"],
                side=order["side"],
                order_type=order["order_type"],
                status=order["status"],
                price=order["price"],
                quantity=order["quantity"],
                filled_quantity=order["filled_quantity"],
                avg_fill_price=order["avg_fill_price"],
            )

    def get_active_orders(self) -> list[OrderInfo]:
        """获取所有活跃订单（状态非终态）.

        Returns:
            活跃订单列表
        """
        if self._cpp_available:
            return self._get_active_orders_cpp()
        else:
            active_statuses = ("PENDING_NEW", "NEW", "PARTIAL_FILLED")
            result = []
            for oid, order in self._python_orders.items():
                if order["status"] in active_statuses:
                    result.append(OrderInfo(
                        order_id=oid,
                        symbol=order["symbol"],
                        side=order["side"],
                        order_type=order["order_type"],
                        status=order["status"],
                        price=order["price"],
                        quantity=order["quantity"],
                        filled_quantity=order["filled_quantity"],
                        avg_fill_price=order["avg_fill_price"],
                    ))
            return result

    # ------------------------------------------------------------------
    # 模拟成交（回测引擎调用）
    # ------------------------------------------------------------------

    def simulate_fill(
        self,
        order_id: int,
        fill_price: float,
        fill_quantity: int,
    ) -> OrderResult:
        """模拟订单成交（回测引擎在每根 bar 后调用）.

        在 C++ 模式下，通过 OrderManager.on_order_fill() 更新订单状态。
        在 Python 回退模式下，直接更新内部状态。

        Args:
            order_id: 订单 ID
            fill_price: 成交价格
            fill_quantity: 成交数量

        Returns:
            更新后的 OrderResult
        """
        if self._cpp_available:
            _cpp_call_void(
                self._order_manager.on_order_fill,
                order_id, fill_quantity, int(fill_price * 10000),
            )
            order_info = self._get_order_cpp(order_id)
            if order_info is None:
                return OrderResult(order_id=order_id, status="UNKNOWN")
            return OrderResult(
                order_id=order_id,
                status=order_info.status,
                filled_qty=order_info.filled_quantity,
                fill_price=order_info.avg_fill_price,
            )
        else:
            order = self._python_orders.get(order_id)
            if order is None:
                return OrderResult(order_id=order_id, status="UNKNOWN")

            order["filled_quantity"] += fill_quantity
            # 计算平均成交价
            total_filled = order["filled_quantity"]
            prev_avg = order["avg_fill_price"]
            prev_qty = total_filled - fill_quantity
            if total_filled > 0:
                order["avg_fill_price"] = (
                    (prev_avg * prev_qty + fill_price * fill_quantity) / total_filled
                )

            # 更新状态
            if order["filled_quantity"] >= order["quantity"]:
                order["status"] = "FILLED"
            else:
                order["status"] = "PARTIAL_FILLED"

            return OrderResult(
                order_id=order_id,
                status=order["status"],
                filled_qty=order["filled_quantity"],
                fill_price=order["avg_fill_price"],
            )

    def get_order_count(self) -> dict[str, int]:
        """获取订单统计.

        Returns:
            {"total": 总订单数, "active": 活跃订单数}
        """
        if self._cpp_available:
            return {
                "total": self._order_manager.total_order_count,
                "active": self._order_manager.active_order_count,
            }
        else:
            active_statuses = ("PENDING_NEW", "NEW", "PARTIAL_FILLED")
            total = len(self._python_orders)
            active = sum(
                1 for o in self._python_orders.values()
                if o["status"] in active_statuses
            )
            return {"total": total, "active": active}

    # ------------------------------------------------------------------
    # C++ 模式内部方法
    # ------------------------------------------------------------------

    def _submit_order_cpp(
        self,
        symbol: str,
        side: str,
        order_type: str,
        price: float,
        quantity: int,
        stop_price: float,
        time_in_force: str,
    ) -> OrderResult:
        """C++ 模式: 通过 OrderManager + MockBroker 提交订单.

        流程:
        1. OrderManager.create_order() 创建订单
        2. MockBroker.submit_order(order) 提交到模拟经纪商
        3. OrderManager.on_order_accepted() 接受订单
        4. MARKET 单: OrderManager.on_order_fill() 立即成交
        """
        from quant_invest._quant_core import OrderRequest

        req = OrderRequest()
        req.symbol = symbol
        req.side = _SIDE_CPP_MAP.get(side, _SIDE_CPP_MAP["BUY"])
        req.type = _ORDER_TYPE_CPP_MAP.get(order_type, _ORDER_TYPE_CPP_MAP["MARKET"])
        req.tif = _TIF_CPP_MAP.get(time_in_force, _TIF_CPP_MAP["DAY"])
        req.price = int(price * 10000)  # C++ 使用整数价格（万分之一元）
        req.quantity = quantity
        if stop_price > 0:
            req.stop_price = int(stop_price * 10000)

        order_id = self._order_manager.create_order(req)

        if order_id is None:
            return OrderResult(status="REJECTED", reject_reason="create_order returned None")

        # 提交到 MockBroker
        order = self._order_manager.find_order(order_id)
        if order is not None and self._broker is not None:
            self._broker.submit_order(order)

        # 接受订单（broker_order_id 为字符串）
        _cpp_call_void(self._order_manager.on_order_accepted, order_id, str(order_id))

        # MARKET 单: 立即成交
        if order_type == "MARKET" and price > 0:
            _cpp_call_void(
                self._order_manager.on_order_fill,
                order_id, quantity, int(price * 10000),
            )

        order_info = self._get_order_cpp(order_id)
        if order_info is None:
            return OrderResult(order_id=order_id, status="UNKNOWN")

        return OrderResult(
            order_id=order_id,
            status=order_info.status,
            filled_qty=order_info.filled_quantity,
            fill_price=order_info.avg_fill_price,  # _get_order_cpp 已做 /10000 转换
        )

    def _get_order_cpp(self, order_id: int) -> OrderInfo | None:
        """C++ 模式: 查询订单信息."""
        order = self._order_manager.find_order(order_id)
        if order is None:
            return None

        status_str = _STATUS_CPP_TO_STR.get(order.status, "UNKNOWN")
        side_str = "BUY" if order.side.value == 0 else "SELL"

        # 订单类型映射
        type_val = order.type.value
        type_str = {0: "MARKET", 1: "LIMIT", 2: "STOP", 3: "STOP_LIMIT"}.get(type_val, "UNKNOWN")

        return OrderInfo(
            order_id=order.order_id,
            symbol=order.symbol,
            side=side_str,
            order_type=type_str,
            status=status_str,
            price=order.price / 10000.0 if order.price > 0 else 0.0,
            quantity=order.quantity,
            filled_quantity=order.filled_quantity,
            avg_fill_price=order.avg_fill_price / 10000.0 if order.avg_fill_price > 0 else 0.0,
            reject_reason=order.reject_reason,
        )

    def _get_active_orders_cpp(self) -> list[OrderInfo]:
        """C++ 模式: 获取活跃订单."""
        from quant_invest._quant_core import OrderStatus

        # 查询活跃状态（不含 PENDING_CANCEL，因 find_orders_by_status 对该状态
        # 存在 pybind11 对象生命周期问题导致段错误）
        active_statuses = [
            OrderStatus.PENDING_NEW,
            OrderStatus.NEW,
            OrderStatus.PARTIAL_FILLED,
        ]

        result = []
        for status in active_statuses:
            orders = self._order_manager.find_orders_by_status(status)
            for order in orders:
                status_str = _STATUS_CPP_TO_STR.get(order.status, "UNKNOWN")
                side_str = "BUY" if order.side.value == 0 else "SELL"
                type_val = order.type.value
                type_str = {0: "MARKET", 1: "LIMIT", 2: "STOP", 3: "STOP_LIMIT"}.get(type_val, "UNKNOWN")

                result.append(OrderInfo(
                    order_id=order.order_id,
                    symbol=order.symbol,
                    side=side_str,
                    order_type=type_str,
                    status=status_str,
                    price=order.price / 10000.0 if order.price > 0 else 0.0,
                    quantity=order.quantity,
                    filled_quantity=order.filled_quantity,
                    avg_fill_price=order.avg_fill_price / 10000.0 if order.avg_fill_price > 0 else 0.0,
                ))

        return result

    # ------------------------------------------------------------------
    # 纯 Python 回退内部方法
    # ------------------------------------------------------------------

    def _submit_order_python(
        self,
        symbol: str,
        side: str,
        order_type: str,
        price: float,
        quantity: int,
    ) -> OrderResult:
        """纯 Python 回退: 模拟订单提交."""
        order_id = self._python_next_id
        self._python_next_id += 1

        # MARKET 单立即成交
        if order_type == "MARKET" and price > 0:
            status = "FILLED"
            filled_qty = quantity
            fill_price = price
        else:
            status = "NEW"
            filled_qty = 0
            fill_price = 0.0

        self._python_orders[order_id] = {
            "symbol": symbol,
            "side": side,
            "order_type": order_type,
            "status": status,
            "price": price,
            "quantity": quantity,
            "filled_quantity": filled_qty,
            "avg_fill_price": fill_price,
        }

        return OrderResult(
            order_id=order_id,
            status=status,
            filled_qty=filled_qty,
            fill_price=fill_price,
        )