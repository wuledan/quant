"""滑点模型"""

from __future__ import annotations


def percentage_slippage(price: float, slippage_rate: float, direction: str) -> float:
    """按比例滑点"""
    if direction == "BUY":
        return price * (1 + slippage_rate)
    else:
        return price * (1 - slippage_rate)


def fixed_slippage(price: float, slippage_amount: float, direction: str) -> float:
    """固定滑点"""
    if direction == "BUY":
        return price + slippage_amount
    else:
        return price - slippage_amount
