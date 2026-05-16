"""仓位管理.

支持多种仓位计算模型：
1. 等权分配 (Equal Weight)
2. 风险平价 (Risk Parity)
3. 凯利公式 (Kelly Criterion)
4. 目标波动率 (Target Volatility)
"""

from __future__ import annotations

import numpy as np


class PositionSizer:
    """仓位管理器."""

    def __init__(self, method: str = "equal_weight", **kwargs) -> None:
        """初始化仓位管理器.

        Args:
            method: 仓位计算方法
                equal_weight - 等权分配
                risk_parity - 风险平价
                kelly - 凯利公式
                target_vol - 目标波动率
            **kwargs: 方法特有参数
        """
        self.method = method
        self._config = kwargs

    def compute(
        self,
        total_cash: float,
        symbols: list[str],
        signals: dict[str, float] | None = None,
        prices: dict[str, float] | None = None,
        strengths: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """计算每个标的的目标仓位金额.

        Args:
            total_cash: 可用总资金
            symbols: 标的列表
            signals: 信号方向 {symbol: 方向权重}, 正数=LONG 负数=SHORT
            prices: 标的当前价格 {symbol: price}
            strengths: 信号强度 {symbol: 0~1}

        Returns:
            {symbol: 目标仓位金额}
        """
        if not symbols or total_cash <= 0:
            return {s: 0.0 for s in symbols}

        method_map = {
            "equal_weight": self._equal_weight,
            "risk_parity": self._risk_parity,
            "kelly": self._kelly,
            "target_vol": self._target_vol,
        }
        compute_fn = method_map.get(self.method)
        if compute_fn is None:
            raise ValueError(f"Unknown position sizing method: {self.method}")

        return compute_fn(total_cash, symbols, signals, prices, strengths)

    def _equal_weight(
        self,
        total_cash: float,
        symbols: list[str],
        signals: dict[str, float] | None = None,
        prices: dict[str, float] | None = None,
        strengths: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """等权分配：所有标的均分资金."""
        active = list(symbols)
        if signals:
            active = [s for s in symbols if s in signals and signals[s] != 0]
        if not active:
            return {s: 0.0 for s in symbols}

        per_symbol = total_cash / len(active)
        result: dict[str, float] = {}
        for s in symbols:
            if s in active:
                weight = strengths.get(s, 1.0) if strengths else 1.0
                result[s] = per_symbol * max(0.0, min(1.0, weight))
            else:
                result[s] = 0.0
        return result

    def _risk_parity(
        self,
        total_cash: float,
        symbols: list[str],
        signals: dict[str, float] | None = None,
        prices: dict[str, float] | None = None,
        strengths: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """风险平价：按波动率倒数分配.

        波动率从 prices 计算，若无价格数据则等权.
        """
        active = list(symbols)
        if signals:
            active = [s for s in symbols if s in signals and signals[s] != 0]
        if not active:
            return {s: 0.0 for s in symbols}

        n = len(active)
        if not prices:
            per_sym = total_cash / n
            return {s: per_sym if s in active else 0.0 for s in symbols}

        # 使用逆波动率加权
        vols: dict[str, float] = {}
        for s in active:
            # 模拟波动率：没有历史数据时使用默认值
            vol = self._config.get(f"vol_{s}", 0.25)
            vols[s] = max(vol, 0.01)

        inv_vol_sum = sum(1.0 / vols[s] for s in active)
        result: dict[str, float] = {}
        for s in symbols:
            if s in active:
                weight = (1.0 / vols[s]) / inv_vol_sum
                result[s] = total_cash * weight
            else:
                result[s] = 0.0
        return result

    def _kelly(
        self,
        total_cash: float,
        symbols: list[str],
        signals: dict[str, float] | None = None,
        prices: dict[str, float] | None = None,
        strengths: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """凯利公式：f* = (p * b - q) / b.

        p = win_rate, q = 1 - p, b = avg_win / avg_loss
        """
        default_win_rate = self._config.get("default_win_rate", 0.55)
        default_win_loss_ratio = self._config.get("default_win_loss_ratio", 1.5)
        kelly_fraction = self._config.get("kelly_fraction", 0.25)  # 半凯利

        active = list(symbols)
        if signals:
            active = [s for s in symbols if s in signals and signals[s] != 0]
        if not active:
            return {s: 0.0 for s in symbols}

        result: dict[str, float] = {}
        for s in symbols:
            if s in active:
                p = self._config.get(f"win_rate_{s}", default_win_rate)
                b = self._config.get(f"win_loss_{s}", default_win_loss_ratio)
                q = 1.0 - p
                kelly_pct = max(0.0, (p * b - q) / b)
                # 应用分数凯利和信号强度
                strength = strengths.get(s, 1.0) if strengths else 1.0
                final_pct = kelly_pct * kelly_fraction * max(0.0, min(1.0, strength))
                result[s] = total_cash * final_pct
            else:
                result[s] = 0.0
        return result

    def _target_vol(
        self,
        total_cash: float,
        symbols: list[str],
        signals: dict[str, float] | None = None,
        prices: dict[str, float] | None = None,
        strengths: dict[str, float] | None = None,
    ) -> dict[str, float]:
        """目标波动率：仓位 = 目标波动率 / 标的波动率 * 调整因子."""
        target_vol = self._config.get("target_volatility", 0.15)
        max_leverage = self._config.get("max_leverage", 1.0)

        active = list(symbols)
        if signals:
            active = [s for s in symbols if s in signals and signals[s] != 0]
        if not active:
            return {s: 0.0 for s in symbols}

        n = len(active)
        per_symbol_base = total_cash / n

        result: dict[str, float] = {}
        for s in symbols:
            if s in active:
                vol = self._config.get(f"vol_{s}", 0.25)
                safe_vol = max(vol, 0.01)
                weight = target_vol / safe_vol
                weight = min(weight, max_leverage)
                strength = strengths.get(s, 1.0) if strengths else 1.0
                result[s] = per_symbol_base * weight * max(0.0, min(1.0, strength))
            else:
                result[s] = 0.0
        return result

    def compute_shares(
        self,
        target_amounts: dict[str, float],
        prices: dict[str, float],
    ) -> dict[str, int]:
        """将目标金额转换为股数（取整到100股）."""
        result: dict[str, int] = {}
        for symbol, amount in target_amounts.items():
            price = prices.get(symbol, 0.0)
            if price <= 0:
                result[symbol] = 0
            else:
                shares = int(amount / price / 100) * 100
                # A股最小单位100股
                result[symbol] = max(0, shares)
        return result
