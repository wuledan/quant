#!/usr/bin/env python3
"""PerformanceAnalyzer绩效分析测试."""

from __future__ import annotations

import pandas as pd
import pytest

from quant_invest.backtest.performance import PerformanceAnalyzer, PerformanceMetrics


class TestPerformanceAnalyzer:
    """PerformanceAnalyzer单元测试."""

    @pytest.fixture
    def analyzer(self) -> PerformanceAnalyzer:
        return PerformanceAnalyzer()

    @pytest.fixture
    def sample_equity(self) -> pd.Series:
        """构建单调递增净值序列."""
        dates = pd.bdate_range("2024-01-02", "2024-06-28")
        n = len(dates)
        values = [1_000_000 * (1 + 0.0003 * i) for i in range(n)]
        return pd.Series(values, index=dates)

    @pytest.fixture
    def sample_snapshots(self) -> pd.DataFrame:
        """构建示例组合快照."""
        dates = pd.bdate_range("2024-01-02", "2024-06-28")
        n = len(dates)
        nav = [1_000_000 * (1 + 0.0003 * i) for i in range(n)]
        return pd.DataFrame(
            {
                "total_value": nav,
                "cash": [v * 0.1 for v in nav],
                "positions_value": [v * 0.9 for v in nav],
                "positions_count": [10] * n,
            },
            index=pd.DatetimeIndex(dates, name="date"),
        )

    def test_calc_max_drawdown_no_drawdown(self, sample_equity):
        """无回撤."""
        max_dd, duration = PerformanceAnalyzer.calc_max_drawdown(sample_equity)
        assert max_dd >= 0
        assert duration == 0

    def test_calc_max_drawdown_with_drawdown(self):
        """有回撤."""
        dates = pd.bdate_range("2024-01-02", "2024-06-28")
        n = len(dates)
        values = []
        for i in range(n):
            if i < 50:
                v = 1_000_000 * (1 + 0.001 * i)
            elif i < 80:
                v = 1_100_000 - (i - 50) * 10000
            else:
                v = 800_000 + (i - 80) * 5000
            values.append(v)
        equity = pd.Series(values, index=dates)

        max_dd, duration = PerformanceAnalyzer.calc_max_drawdown(equity)
        assert max_dd < -0.2
        assert duration > 0

    def test_calc_sharpe_ratio(self, analyzer):
        """夏普比率计算."""
        dates = pd.bdate_range("2024-01-02", "2024-06-28")
        n = len(dates)
        # 0.1% daily return with small noise
        import numpy as np
        np.random.seed(42)
        returns = pd.Series(0.001 + np.random.randn(n) * 0.005, index=dates)

        sharpe = analyzer.calc_sharpe_ratio(returns, risk_free_rate=0.03)
        assert sharpe > 0
        assert isinstance(sharpe, float)

    def test_calc_sharpe_ratio_zero_vol(self, analyzer):
        """零波动率场景."""
        returns = pd.Series([0.001] * 10)
        sharpe = analyzer.calc_sharpe_ratio(returns, risk_free_rate=0.03)
        assert sharpe == 0.0

    def test_analyze_full(self, analyzer, sample_snapshots):
        """完整分析."""
        benchmark = pd.Series(dtype=float)
        metrics = analyzer.analyze(sample_snapshots, benchmark)

        assert isinstance(metrics, PerformanceMetrics)
        assert metrics.total_return > 0
        assert isinstance(metrics.sharpe_ratio, float)
        assert metrics.total_trades >= 0
        assert 0 <= metrics.win_rate <= 1

    def test_analyze_invalid_empty(self, analyzer):
        """空数据应抛出异常."""
        with pytest.raises(ValueError, match="Empty portfolio snapshots"):
            analyzer.analyze(pd.DataFrame(), pd.Series(dtype=float))

    def test_calc_turnover(self, analyzer, sample_snapshots):
        """换手率计算."""
        turnover = analyzer.calc_turnover(sample_snapshots)
        assert turnover >= 0

    def test_calc_turnover_empty(self, analyzer):
        """空数据换手率."""
        turnover = analyzer.calc_turnover(pd.DataFrame())
        assert turnover == 0.0

    def test_generate_report(self, analyzer, sample_snapshots):
        """生成报告."""
        benchmark = pd.Series(dtype=float)
        metrics = analyzer.analyze(sample_snapshots, benchmark)
        report = analyzer.generate_report(metrics)

        assert isinstance(report, str)
        assert "回测绩效分析报告" in report

    def test_generate_report_to_file(self, analyzer, sample_snapshots, tmp_path):
        """输出报告到文件."""
        benchmark = pd.Series(dtype=float)
        metrics = analyzer.analyze(sample_snapshots, benchmark)
        output = str(tmp_path / "report.txt")
        analyzer.generate_report(metrics, output_path=output)

        content = (tmp_path / "report.txt").read_text()
        assert "回测绩效分析报告" in content
