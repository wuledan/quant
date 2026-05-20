#!/usr/bin/env python3
"""风控引擎适配器 — 封装 C++ RiskEngine，为管道提供风控检查.

RiskEngineAdapter 封装 C++ RiskEngine:
1. check_order(): 订单风控检查
2. check_portfolio(): 持仓风控检查
3. update_position(): 更新持仓信息

当 C++ RiskEngine 不可用时，回退到纯 Python 风控规则。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any

logger = logging.getLogger("quant_invest.risk.risk_adapter")


@dataclass
class RiskCheckResult:
    """风控检查结果."""

    passed: bool = True
    risk_level: str = "LOW"  # LOW / MEDIUM / HIGH / CRITICAL
    violations: list[str] = None
    warnings: list[str] = None

    def __post_init__(self):
        if self.violations is None:
            self.violations = []
        if self.warnings is None:
            self.warnings = []


class RiskEngineAdapter:
    """风控引擎适配器 — 封装 C++ RiskEngine.

    提供统一的风控检查接口:
    - check_order(): 订单级风控（单笔限额、频率限制）
    - check_portfolio(): 组合级风控（集中度、回撤）
    - update_position(): 更新持仓信息

    当 C++ RiskEngine 不可用时，回退到纯 Python 规则。
    """

    def __init__(self) -> None:
        self._cpp_available = False
        self._cpp_engine: Any = None
        self._connected = False

        # Python 回退: 风控参数
        self._max_order_value: float = 500_000.0  # 单笔最大金额
        self._max_position_ratio: float = 0.3  # 单标的最大持仓比例
        self._max_drawdown: float = 0.1  # 最大回撤
        self._max_order_frequency: int = 10  # 每分钟最大下单数
        self._order_timestamps: list[float] = []

        # 持仓信息
        self._positions: dict[str, float] = {}  # symbol → market_value
        self._total_value: float = 1_000_000.0
        self._peak_value: float = 1_000_000.0

        self._try_init_cpp()

    def _try_init_cpp(self) -> None:
        """尝试初始化 C++ RiskEngine."""
        try:
            from quant_invest._quant_core import RiskEngine as CppRiskEngine

            self._cpp_engine = CppRiskEngine()
            self._cpp_available = True
            logger.info("C++ RiskEngine 初始化成功")
        except ImportError:
            self._cpp_available = False
            logger.info("C++ RiskEngine 不可用，使用纯 Python 风控")

    @property
    def cpp_available(self) -> bool:
        """C++ RiskEngine 是否可用."""
        return self._cpp_available

    # ------------------------------------------------------------------
    # 风控检查
    # ------------------------------------------------------------------

    def check_order(
        self,
        symbol: str,
        side: str,
        price: float,
        quantity: int,
    ) -> RiskCheckResult:
        """订单级风控检查.

        Args:
            symbol: 标的代码
            side: 买卖方向
            price: 价格
            quantity: 数量

        Returns:
            RiskCheckResult
        """
        if self._cpp_available:
            return self._check_order_cpp(symbol, side, price, quantity)
        else:
            return self._check_order_python(symbol, side, price, quantity)

    def check_portfolio(self) -> RiskCheckResult:
        """组合级风控检查.

        Returns:
            RiskCheckResult
        """
        if self._cpp_available:
            return self._check_portfolio_cpp()
        else:
            return self._check_portfolio_python()

    def update_position(
        self,
        symbol: str,
        market_value: float,
        total_value: float,
    ) -> None:
        """更新持仓信息.

        Args:
            symbol: 标的代码
            market_value: 该标的市值
            total_value: 总资产
        """
        self._positions[symbol] = market_value
        self._total_value = total_value
        if total_value > self._peak_value:
            self._peak_value = total_value

    # ------------------------------------------------------------------
    # C++ 模式
    # ------------------------------------------------------------------

    def _check_order_cpp(
        self,
        symbol: str,
        side: str,
        price: float,
        quantity: int,
    ) -> RiskCheckResult:
        """C++ 模式: 通过 RiskEngine 检查订单."""
        try:
            from quant_invest._quant_core import OrderSide

            side_cpp = OrderSide.BUY if side == "BUY" else OrderSide.SELL
            result = self._cpp_engine.check_order(symbol, side_cpp, int(price * 10000), quantity)

            return RiskCheckResult(
                passed=result.passed,
                risk_level=result.risk_level,
                violations=list(result.violations) if hasattr(result, 'violations') else [],
                warnings=list(result.warnings) if hasattr(result, 'warnings') else [],
            )
        except Exception as e:
            logger.warning("C++ RiskEngine check_order 失败: %s，回退到 Python", e)
            return self._check_order_python(symbol, side, price, quantity)

    def _check_portfolio_cpp(self) -> RiskCheckResult:
        """C++ 模式: 通过 RiskEngine 检查组合."""
        try:
            result = self._cpp_engine.check_portfolio()
            return RiskCheckResult(
                passed=result.passed,
                risk_level=result.risk_level,
            )
        except Exception as e:
            logger.warning("C++ RiskEngine check_portfolio 失败: %s，回退到 Python", e)
            return self._check_portfolio_python()

    # ------------------------------------------------------------------
    # Python 回退模式
    # ------------------------------------------------------------------

    def _check_order_python(
        self,
        symbol: str,
        side: str,
        price: float,
        quantity: int,
    ) -> RiskCheckResult:
        """Python 回退: 基本风控规则."""
        violations = []
        warnings = []

        # 单笔金额检查
        order_value = price * quantity
        if order_value > self._max_order_value:
            violations.append(
                f"单笔金额 {order_value:.0f} 超过限额 {self._max_order_value:.0f}"
            )

        # 持仓集中度检查（买入时）
        if side == "BUY" and self._total_value > 0:
            new_position = self._positions.get(symbol, 0) + order_value
            ratio = new_position / self._total_value
            if ratio > self._max_position_ratio:
                violations.append(
                    f"标的 {symbol} 持仓比例 {ratio:.1%} 超过限制 {self._max_position_ratio:.1%}"
                )

        # 下单频率检查
        import time
        now = time.time()
        recent = [t for t in self._order_timestamps if now - t < 60]
        if len(recent) >= self._max_order_frequency:
            violations.append(
                f"下单频率 {len(recent)}/min 超过限制 {self._max_order_frequency}/min"
            )
        self._order_timestamps.append(now)

        # 价格合理性检查
        if price <= 0:
            violations.append("价格不能为 0 或负数")
        if quantity <= 0:
            violations.append("数量不能为 0 或负数")

        risk_level = "LOW"
        if violations:
            risk_level = "HIGH"
        elif warnings:
            risk_level = "MEDIUM"

        return RiskCheckResult(
            passed=len(violations) == 0,
            risk_level=risk_level,
            violations=violations,
            warnings=warnings,
        )

    def _check_portfolio_python(self) -> RiskCheckResult:
        """Python 回退: 组合级风控检查."""
        violations = []
        warnings = []

        # 最大回撤检查
        if self._peak_value > 0:
            drawdown = (self._peak_value - self._total_value) / self._peak_value
            if drawdown > self._max_drawdown:
                violations.append(
                    f"回撤 {drawdown:.1%} 超过限制 {self._max_drawdown:.1%}"
                )
            elif drawdown > self._max_drawdown * 0.8:
                warnings.append(
                    f"回撤 {drawdown:.1%} 接近限制 {self._max_drawdown:.1%}"
                )

        # 集中度检查
        if self._total_value > 0:
            for symbol, value in self._positions.items():
                ratio = value / self._total_value
                if ratio > self._max_position_ratio:
                    violations.append(
                        f"标的 {symbol} 持仓比例 {ratio:.1%} 超过限制"
                    )

        risk_level = "LOW"
        if violations:
            risk_level = "HIGH"
        elif warnings:
            risk_level = "MEDIUM"

        return RiskCheckResult(
            passed=len(violations) == 0,
            risk_level=risk_level,
            violations=violations,
            warnings=warnings,
        )
