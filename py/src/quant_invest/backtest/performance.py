#!/usr/bin/env python3
"""绩效分析器"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

import numpy as np
import pandas as pd


@dataclass
class PerformanceMetrics:
    """绩效分析指标"""

    # 收益指标
    total_return: float
    annual_return: float
    benchmark_return: float
    excess_return: float

    # 风险指标
    annual_volatility: float
    max_drawdown: float
    max_drawdown_duration: int
    sharpe_ratio: float
    sortino_ratio: float
    calmar_ratio: float

    # 交易指标
    total_trades: int
    win_rate: float
    profit_loss_ratio: float
    turnover_rate: float
    avg_holding_days: float

    # 绩效归因
    monthly_returns: pd.Series
    sector_exposure: pd.DataFrame


class PerformanceAnalyzer:
    """绩效分析器"""

    def analyze(
        self,
        portfolio_snapshots: pd.DataFrame,
        benchmark_series: pd.Series,
        risk_free_rate: float = 0.03,
    ) -> PerformanceMetrics:
        """计算完整绩效指标"""
        if portfolio_snapshots.empty:
            raise ValueError("Empty portfolio snapshots")

        # 提取净值序列
        if "total_value" in portfolio_snapshots.columns:
            equity_curve = portfolio_snapshots["total_value"]
        else:
            equity_curve = portfolio_snapshots.iloc[:, 0]

        # 计算日收益率
        daily_returns = equity_curve.pct_change().dropna()

        if len(daily_returns) < 2:
            raise ValueError("Not enough data points for analysis")

        # 收益指标
        total_return = float(equity_curve.iloc[-1] / equity_curve.iloc[0] - 1)
        n_days = len(daily_returns)
        annual_return = float((1 + total_return) ** (252 / n_days) - 1) if n_days > 0 else 0.0

        # 基准收益
        if not benchmark_series.empty:
            benchmark_return = float(benchmark_series.iloc[-1] / benchmark_series.iloc[0] - 1)
        else:
            benchmark_return = 0.0

        excess_return = total_return - benchmark_return

        # 风险指标
        annual_volatility = float(daily_returns.std() * np.sqrt(252))

        max_dd, max_dd_duration = self.calc_max_drawdown(equity_curve)

        sharpe = self.calc_sharpe_ratio(daily_returns, risk_free_rate)

        # Sortino
        downside_returns = daily_returns[daily_returns < 0]
        downside_std = downside_returns.std() * np.sqrt(252) if len(downside_returns) > 0 else 1.0
        sortino = float((annual_return - risk_free_rate) / downside_std) if downside_std > 0 else 0.0

        calmar = float(annual_return / abs(max_dd)) if max_dd != 0 else 0.0

        # 交易指标（从持仓变化推断）
        if "positions_count" in portfolio_snapshots.columns:
            pos_changes = portfolio_snapshots["positions_count"].diff().abs().sum()
            total_trades = int(pos_changes)
        else:
            total_trades = 0

        win_rate = float((daily_returns > 0).mean())

        profit_loss_ratio = self._calc_profit_loss_ratio(daily_returns)

        turnover = self.calc_turnover(portfolio_snapshots)

        avg_holding = self._calc_avg_holding_days(daily_returns)

        # 月度收益率
        monthly_returns = daily_returns.groupby(
            [daily_returns.index.year, daily_returns.index.month]
        ).apply(lambda x: (1 + x).prod() - 1)
        monthly_returns.index = [
            f"{int(y)}-{int(m):02d}" for y, m in monthly_returns.index
        ]

        sector_exposure = pd.DataFrame()

        return PerformanceMetrics(
            total_return=total_return,
            annual_return=annual_return,
            benchmark_return=benchmark_return,
            excess_return=excess_return,
            annual_volatility=annual_volatility,
            max_drawdown=max_dd,
            max_drawdown_duration=max_dd_duration,
            sharpe_ratio=sharpe,
            sortino_ratio=sortino,
            calmar_ratio=calmar,
            total_trades=total_trades,
            win_rate=win_rate,
            profit_loss_ratio=profit_loss_ratio,
            turnover_rate=turnover,
            avg_holding_days=avg_holding,
            monthly_returns=pd.Series(monthly_returns),
            sector_exposure=sector_exposure,
        )

    @staticmethod
    def calc_max_drawdown(equity_curve: pd.Series) -> tuple[float, int]:
        """计算最大回撤及持续天数"""
        if len(equity_curve) < 2:
            return 0.0, 0

        rolling_max = equity_curve.expanding().max()
        drawdown = (equity_curve / rolling_max) - 1
        max_dd = float(drawdown.min())

        # 最大回撤持续天数
        dd_duration = 0
        current_duration = 0
        for d in drawdown:
            if d < 0:
                current_duration += 1
                dd_duration = max(dd_duration, current_duration)
            else:
                current_duration = 0

        return max_dd, dd_duration

    @staticmethod
    def calc_sharpe_ratio(
        returns: pd.Series,
        risk_free_rate: float = 0.03,
        periods: int = 252,
    ) -> float:
        """计算夏普比率"""
        if len(returns) < 2:
            return 0.0
        excess_returns = returns - risk_free_rate / periods
        std = excess_returns.std()
        if std < 1e-10:
            return 0.0
        return float(excess_returns.mean() / std * np.sqrt(periods))

    @staticmethod
    def calc_turnover(portfolio_snapshots: pd.DataFrame) -> float:
        """计算换手率"""
        if portfolio_snapshots.empty or "positions_value" not in portfolio_snapshots.columns:
            return 0.0

        # 用持仓价值变化估算换手
        turnover = (
            portfolio_snapshots["positions_value"].diff().abs().sum()
            / portfolio_snapshots["total_value"].mean()
        )
        return float(turnover) if not np.isnan(turnover) else 0.0

    def generate_report(
        self,
        metrics: PerformanceMetrics,
        output_path: str = "",
    ) -> str:
        """生成绩效报告（文本）"""
        lines = [
            "=" * 60,
            "  回测绩效分析报告",
            "=" * 60,
            "",
            "── 收益指标 ──",
            f"  总收益率:      {metrics.total_return:>8.2%}",
            f"  年化收益率:    {metrics.annual_return:>8.2%}",
            f"  基准收益率:    {metrics.benchmark_return:>8.2%}",
            f"  超额收益:      {metrics.excess_return:>8.2%}",
            "",
            "── 风险指标 ──",
            f"  年化波动率:    {metrics.annual_volatility:>8.2%}",
            f"  最大回撤:      {metrics.max_drawdown:>8.2%}",
            f"  最大回撤天数:  {metrics.max_drawdown_duration:>8d}",
            f"  夏普比率:      {metrics.sharpe_ratio:>8.2f}",
            f"  索提诺比率:    {metrics.sortino_ratio:>8.2f}",
            f"  卡玛比率:      {metrics.calmar_ratio:>8.2f}",
            "",
            "── 交易指标 ──",
            f"  总交易次数:    {metrics.total_trades:>8d}",
            f"  胜率:          {metrics.win_rate:>8.2%}",
            f"  盈亏比:        {metrics.profit_loss_ratio:>8.2f}",
            f"  换手率:        {metrics.turnover_rate:>8.2f}",
            f"  平均持仓天数:  {metrics.avg_holding_days:>8.1f}",
            "",
            "=" * 60,
        ]

        report = "\n".join(lines)

        if output_path:
            with open(output_path, "w", encoding="utf-8") as f:
                f.write(report)

        return report

    @staticmethod
    def _calc_profit_loss_ratio(returns: pd.Series) -> float:
        """计算盈亏比"""
        gains = returns[returns > 0]
        losses = returns[returns < 0]
        avg_gain = gains.mean() if len(gains) > 0 else 0.0
        avg_loss = abs(losses.mean()) if len(losses) > 0 else 1.0
        return float(avg_gain / avg_loss) if avg_loss > 0 else 0.0

    @staticmethod
    def _calc_avg_holding_days(returns: pd.Series) -> float:
        """估算平均持仓天数（基于正收益日占比）"""
        if len(returns) == 0:
            return 0.0
        pos_ratio = (returns > 0).mean()
        return float(pos_ratio * len(returns) / 252)
