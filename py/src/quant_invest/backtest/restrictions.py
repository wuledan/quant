"""涨跌停限制"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class LimitRule:
    """涨跌停规则"""

    limit_up_ratio: float = 0.1  # 涨停比例
    limit_down_ratio: float = -0.1  # 跌停比例
    block_buy_at_limit_up: bool = True
    block_sell_at_limit_down: bool = True
    # ST股票涨跌停比例为5%
    st_limit_up_ratio: float = 0.05
    st_limit_down_ratio: float = -0.05


def is_at_limit_up(
    current_price: float,
    prev_close: float,
    limit_ratio: float = 0.1,
) -> bool:
    """判断是否涨停"""
    limit_price = prev_close * (1 + limit_ratio)
    return current_price >= limit_price


def is_at_limit_down(
    current_price: float,
    prev_close: float,
    limit_ratio: float = -0.1,
) -> bool:
    """判断是否跌停"""
    limit_price = prev_close * (1 + limit_ratio)
    return current_price <= limit_price
