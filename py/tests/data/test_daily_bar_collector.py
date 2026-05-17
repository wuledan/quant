#!/usr/bin/env python3
"""DailyBarCollector单元测试."""

from __future__ import annotations

import json
from datetime import date
from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.data.collectors.daily_bar import DailyBarCollector
from quant_invest.data.providers.base import AdjustMethod
from quant_invest.data.quality.validator import QualityLevel


class TestDailyBarCollector:
    """DailyBarCollector 单元测试."""

    @pytest.fixture
    def mock_provider(self) -> MagicMock:
        provider = MagicMock()
        dates = pd.bdate_range("2024-01-02", "2024-01-10")
        df = pd.DataFrame(
            {
                "open": [10.0] * len(dates),
                "high": [11.0] * len(dates),
                "low": [9.5] * len(dates),
                "close": [10.5] * len(dates),
                "volume": [1_000_000] * len(dates),
                "amount": [10_000_000] * len(dates),
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )
        provider.get_daily_bars.return_value = df
        return provider

    @pytest.fixture
    def collector(self, mock_provider: MagicMock, tmp_path: Path) -> DailyBarCollector:
        storage = tmp_path / "daily"
        return DailyBarCollector(
            provider=mock_provider,
            storage_path=str(storage),
            max_retries=2,
            retry_delay=0.01,
        )

    # ── 基本采集流程 ──────────────────────────────────────────

    def test_collect_full(self, collector: DailyBarCollector, mock_provider: MagicMock):
        """全量采集返回正确的DataFrame."""
        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert not result.empty
        assert result.index.name == "trade_date"
        assert all(
            c in result.columns
            for c in ["open", "high", "low", "close", "volume", "amount"]
        )
        mock_provider.get_daily_bars.assert_called_once()

    def test_collect_empty_provider(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """数据源返回空DataFrame不应报错."""
        mock_provider.get_daily_bars.return_value = pd.DataFrame()
        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert result.empty

    def test_collect_parquet_persisted(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """采集后Parquet文件应被写入."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        parquet_path = collector._parquet_path("000001.SZ")
        assert parquet_path.exists()
        loaded = pd.read_parquet(parquet_path)
        assert len(loaded) > 0

    def test_collect_pass_forward_adjust(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """默认复权方式为forward."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        mock_provider.get_daily_bars.assert_called_once()
        call_kwargs = mock_provider.get_daily_bars.call_args[1]
        assert call_kwargs["adjust"] == AdjustMethod.FORWARD

    def test_collect_no_adjust(self, collector: DailyBarCollector):
        """支持不复权."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
            adjust=AdjustMethod.NONE,
        )

    # ── 增量更新 ──────────────────────────────────────────────

    def test_incremental_collect(self, collector: DailyBarCollector, mock_provider: MagicMock):
        """增量采集仅获取进度之后的数据."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert mock_provider.get_daily_bars.call_count == 1

        result2 = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert mock_provider.get_daily_bars.call_count == 1
        assert not result2.empty

    def test_incremental_with_new_data(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """增量采集时数据源有新数据."""
        dates1 = pd.bdate_range("2024-01-02", "2024-01-05")
        df1 = pd.DataFrame(
            {
                "open": [10.0] * len(dates1),
                "high": [11.0] * len(dates1),
                "low": [9.5] * len(dates1),
                "close": [10.5] * len(dates1),
                "volume": [1_000_000] * len(dates1),
                "amount": [10_000_000] * len(dates1),
            },
            index=pd.DatetimeIndex(dates1, name="trade_date"),
        )

        dates2 = pd.bdate_range("2024-01-08", "2024-01-12")
        df2 = pd.DataFrame(
            {
                "open": [11.0] * len(dates2),
                "high": [12.0] * len(dates2),
                "low": [10.5] * len(dates2),
                "close": [11.5] * len(dates2),
                "volume": [2_000_000] * len(dates2),
                "amount": [20_000_000] * len(dates2),
            },
            index=pd.DatetimeIndex(dates2, name="trade_date"),
        )

        mock_provider.get_daily_bars.side_effect = [df1, df2]

        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 5),
        )
        assert mock_provider.get_daily_bars.call_count == 1

        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 12),
        )
        assert mock_provider.get_daily_bars.call_count == 2
        expected_count = len(pd.bdate_range("2024-01-02", "2024-01-12"))
        assert len(result) == expected_count

    # ── 进度持久化 ────────────────────────────────────────────

    def test_progress_persisted(self, collector: DailyBarCollector):
        """采集进度被写入JSON文件."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        progress = collector.get_progress("000001.SZ")
        assert progress == date(2024, 1, 10)

        progress_path = collector._storage_path / "progress.json"
        assert progress_path.exists()
        raw = json.loads(progress_path.read_text())
        assert "000001.SZ" in raw
        assert raw["000001.SZ"]["last_date"] == "2024-01-10"

    def test_progress_none(self, collector: DailyBarCollector):
        """未采集过的标的返回None."""
        assert collector.get_progress("999999.SZ") is None

    def test_clear_progress_single(self, collector: DailyBarCollector):
        """清除单个标的进度."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        collector.clear_progress("000001.SZ")
        assert collector.get_progress("000001.SZ") is None

    def test_clear_progress_all(self, collector: DailyBarCollector):
        """清除所有标的进度."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        collector.clear_progress()
        progress_path = collector._storage_path / "progress.json"
        raw = json.loads(progress_path.read_text())
        assert raw == {}

    def test_get_progress_after_incremental(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """增量采集后进度更新为最新日期."""
        dates1 = pd.bdate_range("2024-01-02", "2024-01-05")
        df1 = pd.DataFrame(
            {
                "open": [10.0] * len(dates1),
                "high": [11.0] * len(dates1),
                "low": [9.5] * len(dates1),
                "close": [10.5] * len(dates1),
                "volume": [1_000_000] * len(dates1),
                "amount": [10_000_000] * len(dates1),
            },
            index=pd.DatetimeIndex(dates1, name="trade_date"),
        )
        mock_provider.get_daily_bars.side_effect = [df1, pd.DataFrame()]

        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 5),
        )
        assert collector.get_progress("000001.SZ") == date(2024, 1, 5)

        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert collector.get_progress("000001.SZ") == date(2024, 1, 5)

    # ── 重试机制 ──────────────────────────────────────────────

    def test_retry_on_failure_then_success(
        self, mock_provider: MagicMock, tmp_path: Path
    ):
        """前两次失败第三次成功."""
        c = DailyBarCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "daily"),
            max_retries=3,
            retry_delay=0.01,
        )
        dates = pd.bdate_range("2024-01-02", "2024-01-03")
        df = pd.DataFrame(
            {
                "open": [10.0] * len(dates),
                "high": [11.0] * len(dates),
                "low": [9.5] * len(dates),
                "close": [10.5] * len(dates),
                "volume": [1_000_000] * len(dates),
                "amount": [10_000_000] * len(dates),
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )
        mock_provider.get_daily_bars.side_effect = [
            ConnectionError("timeout"),
            ConnectionError("timeout"),
            df,
        ]

        result = c.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 3),
        )
        assert not result.empty
        assert mock_provider.get_daily_bars.call_count == 3

    def test_retry_all_fail(self, collector: DailyBarCollector, mock_provider: MagicMock):
        """所有重试都失败应抛出异常."""
        mock_provider.get_daily_bars.side_effect = ConnectionError("API unavailable")

        with pytest.raises(RuntimeError, match="failed after.*retries"):
            collector.collect(
                symbol="000001.SZ",
                start_date=date(2024, 1, 2),
                end_date=date(2024, 1, 3),
            )

    def test_retry_delay_parameters(self, mock_provider: MagicMock, tmp_path: Path):
        """自定义重试参数生效."""
        c = DailyBarCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "daily"),
            max_retries=4,
            retry_delay=0.1,
            retry_backoff=3.0,
        )
        assert c._max_retries == 4
        assert c._retry_delay == 0.1
        assert c._retry_backoff == 3.0

    # ── 数据校验 ──────────────────────────────────────────────

    def test_validation_error_raises(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """数据质量ERROR级别应抛出异常."""
        bad_df = pd.DataFrame(
            {
                "open": [10.0, 10.0],
                "high": [9.5, 9.5],
                "low": [10.5, 10.5],
                "close": [10.0, 10.0],
                "volume": [1_000_000, 1_000_000],
            },
            index=pd.DatetimeIndex(
                pd.bdate_range("2024-01-02", "2024-01-03"), name="trade_date"
            ),
        )
        mock_provider.get_daily_bars.return_value = bad_df

        with pytest.raises(ValueError, match="数据质量校验失败"):
            collector.collect(
                symbol="000001.SZ",
                start_date=date(2024, 1, 2),
                end_date=date(2024, 1, 3),
            )

    def test_validation_warning_ok(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """WARNING级别应允许通过."""
        dates = pd.bdate_range("2024-01-02", "2024-01-10")
        df = pd.DataFrame(
            {
                "open": [10.0] * len(dates),
                "high": [11.0] * len(dates),
                "low": [9.5] * len(dates),
                "close": [10.5] * len(dates),
                "volume": [1_000_000] * len(dates),
                "amount": [10_000_000] * len(dates),
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )
        df.iloc[0, df.columns.get_loc("volume")] = 10_000_000_000
        mock_provider.get_daily_bars.return_value = df

        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )
        assert not result.empty

    # ── 边界情况 ──────────────────────────────────────────────

    def test_multiple_symbols(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """多标的独立存储."""
        dates = pd.bdate_range("2024-01-02", "2024-01-05")

        def side_effect(symbol: str, **kwargs):
            return pd.DataFrame(
                {
                    "open": [10.0] * len(dates),
                    "high": [11.0] * len(dates),
                    "low": [9.5] * len(dates),
                    "close": [10.5] * len(dates),
                    "volume": [1_000_000] * len(dates),
                    "amount": [10_000_000] * len(dates),
                },
                index=pd.DatetimeIndex(dates, name="trade_date"),
            )

        mock_provider.get_daily_bars.side_effect = side_effect

        for sym in ["000001.SZ", "600000.SH"]:
            collector.collect(sym, start_date=date(2024, 1, 2), end_date=date(2024, 1, 5))

        assert collector._parquet_path("000001.SZ").exists()
        assert collector._parquet_path("600000.SH").exists()

    def test_start_after_end(self, collector: DailyBarCollector, mock_provider: MagicMock):
        """起始日期晚于结束日期应返回空."""
        result = collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 2, 1),
            end_date=date(2024, 1, 1),
        )
        assert result.empty
        mock_provider.get_daily_bars.assert_not_called()

    def test_collect_twice_no_duplicate(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """两次采集同一区间不应产生重复行."""
        dates = pd.bdate_range("2024-01-02", "2024-01-04")
        df = pd.DataFrame(
            {
                "open": [10.0] * len(dates),
                "high": [11.0] * len(dates),
                "low": [9.5] * len(dates),
                "close": [10.5] * len(dates),
                "volume": [1_000_000] * len(dates),
                "amount": [10_000_000] * len(dates),
            },
            index=pd.DatetimeIndex(dates, name="trade_date"),
        )
        mock_provider.get_daily_bars.return_value = df

        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 4),
        )
        collector.clear_progress("000001.SZ")
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 4),
        )

        parquet_path = collector._parquet_path("000001.SZ")
        loaded = pd.read_parquet(parquet_path)
        assert len(loaded) == len(dates)

    # ── 自定义Validator ──────────────────────────────────────

    def test_custom_validator(self, mock_provider: MagicMock, tmp_path: Path):
        """传入自定义Validator."""
        mock_validator = MagicMock()
        mock_validator.validate.return_value.level = QualityLevel.PASS

        c = DailyBarCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "daily"),
            validator=mock_validator,
        )
        c.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 4),
        )
        mock_validator.validate.assert_called_once()

    # ── 进度恢复 ──────────────────────────────────────────────

    def test_progress_recovery(self, collector: DailyBarCollector):
        """重启后进度JSON仍可读取."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 10),
        )

        new_collector = DailyBarCollector(
            provider=collector.provider,
            storage_path=str(collector._storage_path),
        )
        assert new_collector.get_progress("000001.SZ") == date(2024, 1, 10)

    def test_incremental_recovery(
        self, collector: DailyBarCollector, mock_provider: MagicMock
    ):
        """重启后增量采集正确."""
        collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 5),
        )
        assert mock_provider.get_daily_bars.call_count == 1

        mock_provider.reset_mock()
        new_collector = DailyBarCollector(
            provider=mock_provider,
            storage_path=str(collector._storage_path),
            max_retries=2,
        )
        new_collector.collect(
            symbol="000001.SZ",
            start_date=date(2024, 1, 2),
            end_date=date(2024, 1, 5),
        )
        mock_provider.get_daily_bars.assert_not_called()
