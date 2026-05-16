"""信号生成与组合

支持多个子策略的信号合并：
1. union - 简单合并：所有子策略信号的并集
2. vote - 投票合并：超过N个子策略同意才产生信号
3. weighted - 加权合并：按子策略历史表现加权
4. layered - 分层合并：先用因子信号过滤，再用择时信号确认
"""

from __future__ import annotations

from ..backtest.events import SignalEvent
from .base import StrategyBase


class SignalGenerator:
    """信号生成与组合.

    支持多个子策略的信号合并：
    1. union - 简单合并：所有子策略信号的并集
    2. vote - 投票合并：超过N个子策略同意才产生信号
    3. weighted - 加权合并：按子策略历史表现加权
    4. layered - 分层合并：先用因子信号过滤，再用择时信号确认
    """

    def __init__(self, method: str = "union") -> None:
        self.method = method
        self._sub_strategies: list[StrategyBase] = []
        self._weights: list[float] = []

    def add_strategy(self, strategy: StrategyBase, weight: float = 1.0) -> None:
        """添加子策略."""
        self._sub_strategies.append(strategy)
        self._weights.append(weight)

    def combine(self, bar_data: dict, positions: dict) -> list[SignalEvent]:
        """合并多个子策略信号."""
        if not self._sub_strategies:
            return []

        if self.method == "union":
            return self._combine_union(bar_data, positions)
        elif self.method == "vote":
            return self._combine_vote(bar_data, positions)
        elif self.method == "weighted":
            return self._combine_weighted(bar_data, positions)
        elif self.method == "layered":
            return self._combine_layered(bar_data, positions)
        else:
            raise ValueError(f"Unknown combine method: {self.method}")

    def _combine_union(
        self, bar_data: dict, positions: dict
    ) -> list[SignalEvent]:
        """所有子策略信号的并集."""
        all_signals: list[SignalEvent] = []
        seen_symbols: set[str] = set()

        for strategy in self._sub_strategies:
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.symbol not in seen_symbols:
                    all_signals.append(sig)
                    seen_symbols.add(sig.symbol)

        return all_signals

    def _combine_vote(
        self, bar_data: dict, positions: dict
    ) -> list[SignalEvent]:
        """投票合并：超过半数子策略同意才产生信号."""
        min_votes = max(1, len(self._sub_strategies) // 2 + 1)
        symbol_votes: dict[str, list[SignalEvent]] = {}

        for strategy in self._sub_strategies:
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.symbol not in symbol_votes:
                    symbol_votes[sig.symbol] = []
                symbol_votes[sig.symbol].append(sig)

        result: list[SignalEvent] = []
        for symbol, sigs in symbol_votes.items():
            if len(sigs) >= min_votes:
                # 使用平均强度
                avg_strength = sum(s.strength for s in sigs) / len(sigs)
                sigs[0].strength = avg_strength
                result.append(sigs[0])

        return result

    def _combine_weighted(
        self, bar_data: dict, positions: dict
    ) -> list[SignalEvent]:
        """加权合并：按子策略权重加权信号强度."""
        symbol_signals: dict[str, list[tuple[SignalEvent, float]]] = {}

        for strategy, weight in zip(self._sub_strategies, self._weights):
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.symbol not in symbol_signals:
                    symbol_signals[sig.symbol] = []
                symbol_signals[sig.symbol].append((sig, weight))

        result: list[SignalEvent] = []
        weight_sum = sum(self._weights)
        for symbol, sigs in symbol_signals.items():
            weighted_strength = sum(s.strength * w for s, w in sigs) / weight_sum
            # 使用权重最大的信号的方向
            best_sig = max(sigs, key=lambda x: x[1])[0]
            result.append(
                SignalEvent(
                    timestamp=best_sig.timestamp,
                    symbol=symbol,
                    direction=best_sig.direction,
                    strength=weighted_strength,
                    reason=f"weighted_combine({self.method})",
                )
            )

        return result

    def _combine_layered(
        self, bar_data: dict, positions: dict
    ) -> list[SignalEvent]:
        """分层合并：前半部分策略为因子层，后半部分为择时层."""
        if len(self._sub_strategies) < 2:
            return self._combine_union(bar_data, positions)

        mid = len(self._sub_strategies) // 2
        factor_strategies = self._sub_strategies[:mid]
        timing_strategies = self._sub_strategies[mid:]

        # 因子层信号
        factor_symbols: set[str] = set()
        for strategy in factor_strategies:
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.direction in ("LONG", "SHORT"):
                    factor_symbols.add(sig.symbol)

        # 择时层确认
        timing_symbols: set[str] = set()
        for strategy in timing_strategies:
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.direction in ("LONG", "SHORT"):
                    timing_symbols.add(sig.symbol)

        # 交集
        confirmed = factor_symbols & timing_symbols

        # 从因子层抽取确认的信号
        result: list[SignalEvent] = []
        for strategy in factor_strategies:
            signals = strategy.on_bar(bar_data, positions)
            for sig in signals:
                if sig.symbol in confirmed:
                    result.append(sig)
                    break  # 每个策略只取第一个确认的信号

        return result
