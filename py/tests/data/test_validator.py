"""DataValidator数据质量校验测试."""

from __future__ import annotations

from datetime import date

import numpy as np
import pandas as pd
import pytest

from quant_invest.data.quality.validator import DataValidator, QualityLevel


class TestDataValidator:
    """DataValidator综合校验测试."""

    @pytest.fixture
    def valid_daily_df(self) -> pd.DataFrame:
        """构建合法的日线DataFrame."""
        n = 20
        dates = pd.bdate_range("2024-01-02", periods=n)
        np.random.seed(42)
        base = 10.0
        close = base + np.cumsum(np.random.randn(n) * 0.1)
        return pd.DataFrame(
            {
                "open": close - 0.05,
                "high": close + 0.1,
                "low": close - 0.15,
                "close": close,
                "volume": np.random.randint(1_000_000, 10_000_000, n),
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )

    @pytest.fixture
    def invalid_daily_df(self) -> pd.DataFrame:
        """构建有问题的日线DataFrame（high < low等）."""
        n = 10
        dates = pd.bdate_range("2024-01-02", periods=n)
        return pd.DataFrame(
            {
                "open": [10.0] * n,
                "high": [9.5] * n,  # high < low → 错误
                "low": [10.5] * n,
                "close": [10.0] * n,
                "volume": [1_000_000] * n,
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )

    def test_validate_empty_df(self):
        """空DataFrame应返回ERROR级别."""
        validator = DataValidator()
        report = validator.validate(
            pd.DataFrame(),
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 31),
        )
        assert report.level == QualityLevel.ERROR

    def test_validate_valid_data(self, valid_daily_df):
        """合法数据应返回PASS级别."""
        validator = DataValidator()
        report = validator.validate(
            valid_daily_df,
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 31),
        )
        # 合法数据应为PASS或WARNING（有少量异常可接受）
        assert report.level in (QualityLevel.PASS, QualityLevel.WARNING)

    def test_validate_consistency_error(self, invalid_daily_df):
        """OHLC不一致应报告ERROR."""
        validator = DataValidator()
        report = validator.validate(
            invalid_daily_df,
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 15),
        )
        assert report.level == QualityLevel.ERROR
        consistency_checks = [c for c in report.checks if c["name"].startswith("consistency_")]
        assert len(consistency_checks) > 0

    def test_nan_detection(self):
        """缺失值检测."""
        validator = DataValidator()
        df = pd.DataFrame(
            {
                "open": [10.0, 11.0, None, 12.0],
                "high": [10.5, 11.5, 12.5, 12.5],
                "low": [9.8, 10.5, 11.5, 11.8],
                "close": [10.2, 11.2, 12.0, 12.2],
                "volume": [1e6, 2e6, 3e6, 4e6],
            }
        )
        report = validator.validate(
            df, symbol="TEST", start_date=date(2024, 1, 1), end_date=date(2024, 1, 4)
        )
        nan_checks = [c for c in report.checks if c["name"] == "nan_check"]
        assert len(nan_checks) == 1

    def _make_large_nan_df(self) -> pd.DataFrame:
        """构建高缺失率DataFrame."""
        n = 50
        data = {"close": [float(i) for i in range(n)]}
        # 50% NaN
        data["volume"] = [None if i % 2 == 0 else float(i * 100) for i in range(n)]
        data["open"] = [float(i) for i in range(n)]
        data["high"] = [float(i) + 1 for i in range(n)]
        data["low"] = [float(i) - 1 for i in range(n)]
        return pd.DataFrame(data)

    def test_high_nan_ratio(self):
        """高缺失率应返回ERROR."""
        validator = DataValidator({"nan_err_threshold": 0.1})
        df = self._make_large_nan_df()
        report = validator.validate(
            df, symbol="TEST", start_date=date(2024, 1, 1), end_date=date(2024, 3, 1)
        )
        assert report.level == QualityLevel.ERROR
