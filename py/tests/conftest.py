#!/usr/bin/env python3
"""pytest全局配置和fixtures."""

from __future__ import annotations

import pandas as pd
import pytest


@pytest.fixture
def sample_daily_data() -> pd.DataFrame:
    """提供示例日线数据用于测试."""
    dates = pd.bdate_range(start="2024-01-02", end="2024-01-31")
    n = len(dates)
    return pd.DataFrame(
        {
            "open": 10.0 + pd.Series(range(n)).cumsum() * 0.1,
            "high": 10.5 + pd.Series(range(n)).cumsum() * 0.1,
            "low": 9.5 + pd.Series(range(n)).cumsum() * 0.1,
            "close": 10.2 + pd.Series(range(n)).cumsum() * 0.1,
            "volume": [1_000_000 + i * 1000 for i in range(n)],
            "amount": [10_000_000 + i * 10_000 for i in range(n)],
        },
        index=pd.DatetimeIndex(dates, name="trade_date"),
    )


@pytest.fixture
def sample_portfolio_snapshots() -> pd.DataFrame:
    """提供示例组合净值序列用于绩效分析测试."""
    dates = pd.bdate_range(start="2024-01-02", end="2024-06-28")
    n = len(dates)
    nav = 1_000_000 * (
        1 + pd.Series(range(n)) * 0.0005 + 0.01 * pd.Series(range(n)).pipe(lambda s: (s / n) ** 2)
    )
    return pd.DataFrame(
        {
            "total_value": nav,
            "cash": nav * 0.1,
            "positions_value": nav * 0.9,
        },
        index=pd.DatetimeIndex(dates, name="date"),
    )


@pytest.fixture
def sample_signals() -> list[dict]:
    """提供示例交易信号."""
    return [
        {"symbol": "000001.SZ", "direction": "LONG", "strength": 0.8},
        {"symbol": "600000.SH", "direction": "SHORT", "strength": 0.6},
    ]
