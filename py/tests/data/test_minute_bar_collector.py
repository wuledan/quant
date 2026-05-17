#!/usr/bin/env python3
"""MinuteBarCollector单元测试."""

from __future__ import annotations

import json
from datetime import date, datetime
from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.data.collectors.minute_bar import MinuteBarCollector
from quant_invest.data.providers.base import AdjustMethod, DataFreq
from quant_invest.data.quality.validator import QualityLevel


class TestMinuteBarCollector:
    """MinuteBarCollector 单元测试."""

    @pytest.fixture
    def mock_provider(self) -> MagicMock:
        provider = MagicMock()
        times = pd.date_range("2024-01-02 09:30", "2024-01-02 10:30", freq="1min")
        df = pd.DataFrame(
            {
                "open": [10.0] * len(times),
                "high": [10.5] * len(times),
                "low": [9.8] * len(times),
                "close": [10.2] * len(times),
                "volume": [100_000] * len(times),
                "amount": [1_000_000] * len(times),
            },
            index=pd.DatetimeIndex(times),
        )
        provider.get_minute_bars.return_value = df
        return provider

    @pytest.fixture
    def collector(self, mock_provider: MagicMock, tmp_path: Path) -> MinuteBarCollector:
        storage = tmp_path / "minute"
        return MinuteBarCollector(
            provider=mock_provider,
            storage_path=str(storage),
            max_retries=2,
            retry_delay=0.01,
        )

    # ── 基本采集流程 ──────────────────────────────────────────

    def test_collect_return_dataframe(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """采集返回DataFrame."""
        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 3),
        )
        assert not result.empty
        mock_provider.get_minute_bars.assert_called()

    def test_collect_empty_provider(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """数据源返回空DataFrame."""
        mock_provider.get_minute_bars.return_value = pd.DataFrame()
        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 3),
        )
        assert result.empty

    def test_collect_parquet_persisted(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """采集后Parquet文件被写入."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 2),
        )
        path = collector._parquet_path("000001.SZ", DataFreq.MIN_1)
        assert path.exists()
        loaded = pd.read_parquet(path)
        assert len(loaded) > 0

    def test_collect_default_freq(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """默认频率1min."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 2),
        )
        mock_provider.get_minute_bars.assert_called()
        call_kwargs = mock_provider.get_minute_bars.call_args[1]
        assert call_kwargs["freq"] == DataFreq.MIN_1

    # ── 多频率 ────────────────────────────────────────────────

    def test_collect_multiple_frequencies(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """支持多种频率."""
        for freq in [DataFreq.MIN_1, DataFreq.MIN_5, DataFreq.MIN_15, DataFreq.MIN_60]:
            mock_provider.reset_mock()
            sym = "000001.SZ"
            collector.collect(
                symbol=sym,
                start_date=date(2024, 1, 2),
                end_date=date(2024, 1, 2),
                freq=freq,
            )
            call_kwargs = mock_provider.get_minute_bars.call_args[1]
            assert call_kwargs["freq"] == freq

    def test_freq_independent_progress(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """不同频率独立记录进度."""
        mock_provider.get_minute_bars.side_effect = [
            pd.DataFrame({"open": [10.0], "high": [11.0], "low": [9.0], "close": [10.5], "volume": [1000], "amount": [10000]},
                         index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 0)])),
            pd.DataFrame({"open": [10.0], "high": [11.0], "low": [9.0], "close": [10.5], "volume": [1000], "amount": [10000]},
                         index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 5)])),
        ]

        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2), freq=DataFreq.MIN_1)
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2), freq=DataFreq.MIN_5)

        assert collector.get_progress("000001.SZ", DataFreq.MIN_1) == date(2024, 1, 2)
        assert collector.get_progress("000001.SZ", DataFreq.MIN_5) == date(2024, 1, 2)
        assert mock_provider.get_minute_bars.call_count == 2

    # ── 增量更新 ──────────────────────────────────────────────

    def test_incremental_collect(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """增量采集不重复获取已有日期."""
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))
        assert mock_provider.get_minute_bars.call_count == 1

        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))
        assert mock_provider.get_minute_bars.call_count == 1

    def test_incremental_new_day(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """增量采集新日期."""
        df_day1 = pd.DataFrame(
            {"open": [10.0], "high": [11.0], "low": [9.0], "close": [10.5], "volume": [1000], "amount": [10000]},
            index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 0)]),
        )
        df_day2 = pd.DataFrame(
            {"open": [11.0], "high": [12.0], "low": [10.0], "close": [11.5], "volume": [2000], "amount": [20000]},
            index=pd.DatetimeIndex([datetime(2024, 1, 3, 10, 0)]),
        )
        mock_provider.get_minute_bars.side_effect = [df_day1, df_day2]

        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 3))
        assert mock_provider.get_minute_bars.call_count == 2

    # ── 进度持久化 ────────────────────────────────────────────

    def test_progress_persisted(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """进度持久化."""
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 3))
        assert collector.get_progress("000001.SZ") == date(2024, 1, 3)

    def test_progress_none(self, collector: MinuteBarCollector):
        """未采集返回None."""
        assert collector.get_progress("999999.SZ") is None

    def test_clear_progress(self, collector: MinuteBarCollector):
        """清除进度."""
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 3))
        collector.clear_progress("000001.SZ")
        assert collector.get_progress("000001.SZ") is None

    def test_clear_progress_all(self, collector: MinuteBarCollector):
        """清除所有进度."""
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 3))
        collector.clear_progress()
        progress_path = collector._storage_path / "progress.json"
        raw = json.loads(progress_path.read_text())
        assert raw == {}

    # ── 重试机制 ──────────────────────────────────────────────

    def test_retry_then_success(
        self, mock_provider: MagicMock, tmp_path: Path
    ):
        """重试后成功."""
        c = MinuteBarCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "minute"),
            max_retries=3,
            retry_delay=0.01,
        )
        df = pd.DataFrame(
            {"open": [10.0], "high": [11.0], "low": [9.0], "close": [10.5], "volume": [1000], "amount": [10000]},
            index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 0)]),
        )
        mock_provider.get_minute_bars.side_effect = [
            ConnectionError("timeout"),
            ConnectionError("timeout"),
            df,
        ]
        result = c.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))
        assert not result.empty
        assert mock_provider.get_minute_bars.call_count == 3

    def test_retry_all_fail(self, collector: MinuteBarCollector, mock_provider: MagicMock):
        """所有重试失败抛出异常."""
        mock_provider.get_minute_bars.side_effect = ConnectionError("API unavailable")
        with pytest.raises(RuntimeError, match="failed after.*retries"):
            collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))

    # ── 数据校验 ──────────────────────────────────────────────

    def test_validation_error_raises(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """数据质量ERROR抛出异常."""
        bad_df = pd.DataFrame(
            {
                "open": [10.0],
                "high": [9.0],
                "low": [11.0],
                "close": [10.5],
                "volume": [1000],
                "amount": [10000],
            },
            index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 0)]),
        )
        mock_provider.get_minute_bars.return_value = bad_df
        with pytest.raises(ValueError, match="数据质量校验失败"):
            collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))

    def test_custom_validator(self, mock_provider: MagicMock, tmp_path: Path):
        """自定义Validator."""
        mock_validator = MagicMock()
        mock_validator.validate.return_value.level = QualityLevel.PASS
        c = MinuteBarCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "minute"),
            validator=mock_validator,
        )
        c.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))
        mock_validator.validate.assert_called()

    # ── 流式按日切分 ──────────────────────────────────────────

    def test_stream_by_day(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """多日采集按天分批调用provider."""
        def side_effect(symbol="", start_time=None, end_time=None, freq=None, adjust=None):
            day = start_time.date()
            times = pd.date_range(start_time, end_time, freq="1min")
            return pd.DataFrame(
                {
                    "open": [10.0] * len(times),
                    "high": [11.0] * len(times),
                    "low": [9.0] * len(times),
                    "close": [10.5] * len(times),
                    "volume": [1000] * len(times),
                    "amount": [10000] * len(times),
                },
                index=pd.DatetimeIndex(times),
            )
        mock_provider.get_minute_bars.side_effect = side_effect

        weekday_count = len(pd.bdate_range("2024-01-02", "2024-01-05"))
        result = collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 5))
        assert mock_provider.get_minute_bars.call_count == weekday_count
        assert not result.empty

    def test_stream_non_trading_day(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """周末不请求provider."""
        result = collector.collect("000001.SZ", date(2024, 1, 6), date(2024, 1, 7))
        mock_provider.get_minute_bars.assert_not_called()
        assert result.empty

    # ── 进度恢复 ──────────────────────────────────────────────

    def test_progress_recovery(self, collector: MinuteBarCollector):
        """重启后进度恢复."""
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 3))
        new_c = MinuteBarCollector(
            provider=collector.provider,
            storage_path=str(collector._storage_path),
        )
        assert new_c.get_progress("000001.SZ") == date(2024, 1, 3)

    def test_collect_twice_no_duplicate(
        self, collector: MinuteBarCollector, mock_provider: MagicMock
    ):
        """两次采集无重复行."""
        df = pd.DataFrame(
            {"open": [10.0], "high": [11.0], "low": [9.0], "close": [10.5], "volume": [1000], "amount": [10000]},
            index=pd.DatetimeIndex([datetime(2024, 1, 2, 10, 0)]),
        )
        mock_provider.get_minute_bars.return_value = df

        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))
        collector.clear_progress("000001.SZ")
        collector.collect("000001.SZ", date(2024, 1, 2), date(2024, 1, 2))

        path = collector._parquet_path("000001.SZ", DataFreq.MIN_1)
        loaded = pd.read_parquet(path)
        assert len(loaded) == 1
