#!/usr/bin/env python3
"""AkshareProvider数据采集测试.

使用mock模拟akshare API调用，避免网络依赖。
另外测试DataProviderFactory.
"""

from __future__ import annotations

from datetime import date, datetime
from unittest.mock import MagicMock, patch

import pandas as pd
import pytest

from quant_invest.data.providers import (
    AkshareProvider,
    DataProviderFactory,
)
from quant_invest.data.providers.akshare_provider import RateLimiter
from quant_invest.data.providers.base import AdjustMethod, DataFreq


class TestAkshareProvider:
    """AkshareProvider单元测试."""

    @pytest.fixture
    def provider(self) -> AkshareProvider:
        return AkshareProvider(config={"calls_per_second": 100})

    @patch("akshare.stock_zh_a_hist")
    def test_get_daily_bars(self, mock_hist: MagicMock, provider: AkshareProvider):
        """获取日线数据."""
        dates = pd.bdate_range("2024-01-02", "2024-01-10")
        mock_df = pd.DataFrame(
            {
                "日期": [d.strftime("%Y-%m-%d") for d in dates],
                "开盘": [10.0] * len(dates),
                "收盘": [10.5] * len(dates),
                "最高": [11.0] * len(dates),
                "最低": [9.5] * len(dates),
                "成交量": [1_000_000] * len(dates),
                "成交额": [10_000_000] * len(dates),
                "换手率": [0.01] * len(dates),
            }
        )
        mock_hist.return_value = mock_df

        result = provider.get_daily_bars(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )

        assert not result.empty
        assert result.index.name == "trade_date"
        assert all(c in result.columns for c in ["open", "high", "low", "close", "volume"])
        mock_hist.assert_called_once()

    @patch("akshare.stock_zh_a_hist_min_em")
    def test_get_minute_bars(self, mock_min: MagicMock, provider: AkshareProvider):
        """获取分钟线数据."""
        times = pd.date_range("2024-01-02 09:30", "2024-01-02 10:30", freq="1min")
        mock_df = pd.DataFrame(
            {
                "时间": [t.strftime("%Y-%m-%d %H:%M:%S") for t in times],
                "开盘": [10.0] * len(times),
                "收盘": [10.2] * len(times),
                "最高": [10.5] * len(times),
                "最低": [9.8] * len(times),
                "成交量": [100_000] * len(times),
                "成交额": [1_000_000] * len(times),
            }
        )
        mock_min.return_value = mock_df

        result = provider.get_minute_bars(
            symbol="000001.SZ",
            start_time=datetime(2024, 1, 2, 9, 30),
            end_time=datetime(2024, 1, 2, 10, 30),
        )

        assert not result.empty
        assert len(result) == len(times)
        mock_min.assert_called_once()

    @patch("akshare.tool_trade_date_hist_sina")
    def test_get_trade_calendar(self, mock_cal: MagicMock, provider: AkshareProvider):
        """获取交易日历."""
        mock_cal.return_value = pd.DataFrame(
            {
                "trade_date": pd.bdate_range("2024-01-01", "2024-12-31"),
                "open": [1] * 262,
                "close": [1] * 262,
            }
        )

        result = provider.get_trade_calendar(
            start_date=date(2024, 6, 1),
            end_date=date(2024, 6, 30),
        )

        assert len(result) > 0
        assert all(isinstance(d, date) for d in result)

    def test_name(self, provider: AkshareProvider):
        """数据源名称."""
        assert provider.name == "akshare"

    def test_health_check_fail(self, provider: AkshareProvider):
        """健康检查失败（无网络时）."""
        with patch("akshare.tool_trade_date_hist_sina", side_effect=Exception("No network")):
            assert not provider.health_check()


class TestRateLimiter:
    """频率限制器测试."""

    def test_rate_limiter_high_limit(self):
        """高频率限制不应阻塞."""
        limiter = RateLimiter(calls_per_second=1000)
        import time

        t0 = time.time()
        for _ in range(10):
            limiter.wait()
        elapsed = time.time() - t0
        assert elapsed < 0.5  # 10次调用应很快完成


class TestDataProviderFactory:
    """数据源工厂测试."""

    def test_create_akshare(self):
        """创建akshare数据源."""
        provider = DataProviderFactory.create("akshare")
        assert isinstance(provider, AkshareProvider)
        assert provider.name == "akshare"

    def test_create_unknown(self):
        """未知数据源应抛出异常."""
        with pytest.raises(ValueError, match="Unknown provider"):
            DataProviderFactory.create("unknown")

    def test_register_custom(self):
        """注册自定义数据源."""
        class MockProvider(AkshareProvider):
            @property
            def name(self) -> str:
                return "mock"

        DataProviderFactory.register("mock", MockProvider)
        provider = DataProviderFactory.create("mock")
        assert provider.name == "mock"

    def test_available_providers(self):
        """列出可用数据源."""
        providers = DataProviderFactory.available_providers()
        assert "akshare" in providers

    def test_create_with_config(self):
        """创建数据源时传入配置."""
        provider = DataProviderFactory.create("akshare", config={"calls_per_second": 10})
        assert isinstance(provider, AkshareProvider)
