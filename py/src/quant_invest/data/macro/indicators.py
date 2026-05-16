#!/usr/bin/env python3
"""宏观指标计算：环比/同比/领先指标/复合指标."""

from __future__ import annotations

from datetime import date

import numpy as np
import pandas as pd


class MacroIndicators:
    """宏观指标计算器."""

    @staticmethod
    def calc_mom(series: pd.Series) -> pd.Series:
        """计算环比 (Month-over-Month)."""
        return series.pct_change()

    @staticmethod
    def calc_yoy(series: pd.Series, periods: int = 12) -> pd.Series:
        """计算同比 (Year-over-Year)."""
        return series.pct_change(periods=periods)

    @staticmethod
    def calc_leading_indicator(
        indicators: dict[str, pd.Series],
        weights: dict[str, float] | None = None,
    ) -> pd.Series:
        """合成领先指标."""
        if not indicators:
            return pd.Series(dtype=float)
        names = list(indicators.keys())
        if weights is None:
            w = 1.0 / len(names)
            weights = {k: w for k in names}
        df = pd.DataFrame(indicators)
        df_norm = (df - df.mean()) / df.std()
        result = sum(df_norm[k] * weights[k] for k in names)
        return result

    @staticmethod
    def calc_composite_index(
        series: pd.Series,
        base_date: date | None = None,
        base_value: float = 100.0,
    ) -> pd.Series:
        """计算定基指数."""
        if base_date is not None:
            base_val = series.loc[series.index >= pd.Timestamp(base_date)].iloc[0]
        else:
            base_val = series.iloc[0]
        if base_val == 0:
            return pd.Series(index=series.index, dtype=float)
        return series / base_val * base_value

    @staticmethod
    def calc_moving_average(series: pd.Series, window: int = 3, min_periods: int | None = None) -> pd.Series:
        """移动平均."""
        return series.rolling(window=window, min_periods=min_periods).mean()

    @staticmethod
    def calc_diff_from_trend(series: pd.Series, trend_window: int = 12) -> pd.Series:
        """计算偏离趋势的幅度."""
        trend = series.rolling(window=trend_window).mean()
        return (series - trend) / trend * 100

    @staticmethod
    def classify_regime(gdp_series: pd.Series, cpi_series: pd.Series, pmi_series: pd.Series) -> pd.Series:
        """宏观经济状态分类."""
        common_idx = gdp_series.index.intersection(cpi_series.index).intersection(pmi_series.index)
        if len(common_idx) == 0:
            return pd.Series(index=gdp_series.index, dtype=str)
        gdp_z = (gdp_series - gdp_series.mean()) / gdp_series.std()
        cpi_z = (cpi_series - cpi_series.mean()) / cpi_series.std()
        regimes: list[str] = []
        for idx in common_idx:
            g = gdp_z.loc[idx]
            c = cpi_z.loc[idx]
            if g > 0 and c < 0.5:
                regimes.append("expansion")
            elif g > 0 and c >= 0.5:
                regimes.append("overheating")
            elif g <= 0 and c >= 0.5:
                regimes.append("stagflation")
            else:
                regimes.append("recession")
        return pd.Series(regimes, index=common_idx, name="regime")