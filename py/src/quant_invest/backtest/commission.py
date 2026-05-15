"""手续费模型"""

from __future__ import annotations


def calc_a_share_commission(
    price: float,
    quantity: int,
    direction: str,
    commission_rate: float = 0.00025,
    min_commission: float = 5.0,
    stamp_tax_rate: float = 0.001,
) -> float:
    """计算A股手续费

    Args:
        price: 成交价格
        quantity: 成交数量
        direction: 买入/卖出
        commission_rate: 券商佣金率
        min_commission: 最低佣金
        stamp_tax_rate: 印花税率（仅卖出）

    Returns:
        总手续费
    """
    trade_value = price * quantity
    # 券商佣金（双向）
    commission = max(trade_value * commission_rate, min_commission)
    # 印花税（仅卖出）
    stamp_tax = trade_value * stamp_tax_rate if direction == "SELL" else 0.0
    return commission + stamp_tax
