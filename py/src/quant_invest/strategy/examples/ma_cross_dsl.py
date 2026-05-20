#!/usr/bin/env python3
"""均线交叉策略 — 使用声明式 DSL 定义.

演示:
1. Factor 声明因子
2. cross_above / cross_below 信号组合
3. on_signal 回调处理交易逻辑
4. 与传统 on_bar 策略的对比（见同目录 ma_cross_on_bar.py）
"""

from quant_invest.strategy.dsl import (
    Factor,
    SignalContext,
    Strategy,
    cross_above,
    cross_below,
    strategy,
)


@strategy("ma_cross_dsl")
class MACross(Strategy):
    """双均线交叉策略 (DSL 版).

    逻辑:
    - fast_ma 上穿 slow_ma (金叉) → 买入
    - fast_ma 下穿 slow_ma (死叉) → 卖出
    """

    # ---- 因子声明 ----
    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")

    # ---- 信号声明 ----
    golden_cross = cross_above(fast_ma, slow_ma)  # 金叉信号
    death_cross = cross_below(fast_ma, slow_ma)   # 死叉信号

    def on_signal(self, ctx: SignalContext) -> None:
        """信号触发回调."""
        # 金叉: 买入 95% 仓位
        if self.golden_cross > 0:
            buy_qty = ctx.cash * 0.95 / ctx.price if ctx.price > 0 else 0
            ctx.order(
                symbol=ctx.symbol,
                side="BUY",
                quantity=buy_qty,
            )
        # 死叉: 清仓
        elif self.death_cross < 0:
            ctx.order(
                symbol=ctx.symbol,
                side="SELL",
                quantity=ctx.position,
            )


@strategy("ma_cross_dsl_simple")
class MACrossSimple(Strategy):
    """双均线交叉策略 — 简化版，单信号.

    使用单个 cross_above 信号，正值为金叉，负值通过
    cross_below 产生。适合简单场景。
    """

    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    signal = cross_above(fast_ma, slow_ma)

    def on_signal(self, ctx: SignalContext) -> None:
        if self.signal > 0:
            # 金叉买入
            buy_qty = ctx.cash * 0.95 / ctx.price if ctx.price > 0 else 0
            ctx.order(symbol=ctx.symbol, side="BUY", quantity=buy_qty)
        elif self.signal < 0:
            # 死叉卖出
            ctx.order(symbol=ctx.symbol, side="SELL", quantity=ctx.position)


# ---------------------------------------------------------------------------
# 演示: DSL 策略也支持传统 on_bar 模式（向后兼容）
# ---------------------------------------------------------------------------

@strategy("ma_cross_hybrid")
class MACrossHybrid(Strategy):
    """混合策略 — 同时使用 DSL 因子声明和传统 on_bar 回调.

    适用于需要复杂交易逻辑但因子计算仍想声明式定义的场景。
    """

    fast_ma = Factor("SMA", period=5, input="close")
    slow_ma = Factor("SMA", period=20, input="close")
    # 声明因子但不声明信号，在 on_bar 中手动使用因子值

    def on_bar(self, bar_data: dict, positions: dict) -> list:
        """传统 on_bar 回调 — 可访问声明的因子值."""
        # 因子值由引擎计算后通过 set_signal_value 注入
        # 这里演示向后兼容模式
        signals = []
        return signals


# ---------------------------------------------------------------------------
# 演示: 更复杂的策略 — RSI + 均线组合
# ---------------------------------------------------------------------------

@strategy("rsi_ma_combo")
class RSIMACombo(Strategy):
    """RSI + 均线组合策略.

    买入条件: 均线金叉 AND RSI < 70 (未超买)
    卖出条件: 均线死叉 OR RSI > 80 (严重超买)
    """

    fast_ma = Factor("SMA", period=10, input="close")
    slow_ma = Factor("SMA", period=30, input="close")
    rsi = Factor("RSI", period=14, input="close")

    # 信号组合
    golden_cross = cross_above(fast_ma, slow_ma)
    death_cross = cross_below(fast_ma, slow_ma)

    def on_signal(self, ctx: SignalContext) -> None:
        if self.golden_cross > 0:
            # 金叉 + RSI 未超买 → 买入
            ctx.order(symbol=ctx.symbol, side="BUY",
                      quantity=ctx.cash * 0.9 / ctx.price)
        elif self.death_cross < 0:
            # 死叉 → 卖出
            ctx.order(symbol=ctx.symbol, side="SELL",
                      quantity=ctx.position)
