#!/usr/bin/env python3
"""FinancialDataCollector单源测试."""

from __future__ import annotations

import json
from datetime import date, datetime
from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.data.collectors.financial import FinancialDataCollector
from quant_invest.data.quality.validator import QualityLevel


class TestFinancialDataCollector:
    """FinancialDataCollector 单元测试."""

    @pytest.fixture
    def mock_provider(self) -> MagicMock:
        provider = MagicMock()
        dates = pd.date_range("2024-03-31", periods=4, freq="YE")  # 年报
        df = pd.DataFrame(
            {
                "报告期": [d.strftime("%Y-%m-%d") for d in dates],
                "营业收入": [1e8, 1.2e8, 1.5e8, 1.8e8],
                "净利润": [1e7, 1.2e7, 1.5e7, 1.8e7],
                "基本每股收益": [0.5, 0.6, 0.75, 0.9],
            }
        )
        provider.get_financial_data.return_value = df
        return provider

    @pytest.fixture
    def collector(self, mock_provider: MagicMock, tmp_path: Path) -> FinancialDataCollector:
        storage = tmp_path / "financial"
        return FinancialDataCollector(
            provider=mock_provider,
            storage_path=str(storage),
            max_retries=2,
            retry_delay=0.01,
        )

    # ── 基本采集 ──────────────────────────────────────────────

    def test_collect_income(self, collector: FinancialDataCollector):
        """采集利润表."""
        result = collector.collect(
            symbol="000001.SZ",
            report_type="income",
        )
        assert not result.empty
        collector.provider.get_financial_data.assert_called_once()

    def test_collect_balance(self, collector: FinancialDataCollector, mock_provider: MagicMock):
        """采集资产负债表."""
        collector.collect(symbol="000001.SZ", report_type="balance")
        mock_provider.get_financial_data.assert_called()

    def test_collect_cashflow(self, collector: FinancialDataCollector):
        """采集现金流量表."""
        result = collector.collect(symbol="000001.SZ", report_type="cashflow")
        assert not result.empty

    def test_collect_all_report_types(self, collector: FinancialDataCollector):
        """采集全部三大报表."""
        results = collector.collect_all(symbol="000001.SZ")
        assert set(results) == {"income", "balance", "cashflow"}
        for rt, df in results.items():
            assert not df.empty, f"{rt} should not be empty"

    def test_unknown_report_type(self, collector: FinancialDataCollector):
        """未知报表类型抛异常."""
        with pytest.raises(ValueError, match="Unknown report_type"):
            collector.collect(symbol="000001.SZ", report_type="invalid")

    def test_empty_provider(self, collector: FinancialDataCollector, mock_provider: MagicMock):
        """数据源返回空DataFrame."""
        mock_provider.get_financial_data.return_value = pd.DataFrame()
        result = collector.collect(symbol="000001.SZ", report_type="income")
        assert result.empty

    # ── 增量更新 ──────────────────────────────────────────────

    def test_incremental_collect(self, collector: FinancialDataCollector, mock_provider: MagicMock):
        """增量采集不重复获取."""
        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2023, 12, 31))
        assert mock_provider.get_financial_data.call_count == 1

        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2023, 12, 31))
        assert mock_provider.get_financial_data.call_count == 1

    def test_incremental_new_period(self, collector: FinancialDataCollector, mock_provider: MagicMock):
        """增量采集新报告期."""
        df1 = pd.DataFrame({"报告期": ["2023-12-31"], "净利润": [1e7]})
        df2 = pd.DataFrame({"报告期": ["2024-12-31"], "净利润": [1.2e7]})
        mock_provider.get_financial_data.side_effect = [df1, df2]

        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2023, 12, 31))
        assert mock_provider.get_financial_data.call_count == 1

        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2024, 12, 31))
        assert mock_provider.get_financial_data.call_count == 2

    # ── 进度持久化 ────────────────────────────────────────────

    def test_progress_persisted(self, collector: FinancialDataCollector):
        """进度持久化."""
        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2024, 12, 31))
        assert collector.get_progress("000001.SZ", "income") == date(2024, 12, 31)

    def test_progress_none(self, collector: FinancialDataCollector):
        """未采集返回None."""
        assert collector.get_progress("unknown") is None

    def test_clear_progress(self, collector: FinancialDataCollector):
        """清除进度."""
        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2024, 12, 31))
        collector.clear_progress("000001.SZ", "income")
        assert collector.get_progress("000001.SZ", "income") is None

    # ── 重试 ──────────────────────────────────────────────────

    def test_retry_then_success(self, mock_provider: MagicMock, tmp_path: Path):
        """重试后成功."""
        c = FinancialDataCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "fin"),
            max_retries=3,
            retry_delay=0.01,
        )
        df = pd.DataFrame({"报告期": ["2024-12-31"], "净利润": [1e7]})
        mock_provider.get_financial_data.side_effect = [
            ConnectionError("timeout"),
            ConnectionError("timeout"),
            df,
        ]
        result = c.collect(symbol="000001.SZ", report_type="income")
        assert not result.empty
        assert mock_provider.get_financial_data.call_count == 3

    def test_retry_all_fail(self, collector: FinancialDataCollector, mock_provider: MagicMock):
        """所有重试失败抛异常."""
        mock_provider.get_financial_data.side_effect = ConnectionError("API unavailable")
        with pytest.raises(RuntimeError, match="failed after.*retries"):
            collector.collect(symbol="000001.SZ", report_type="income")

    # ── 数据校验 ──────────────────────────────────────────────

    def test_custom_validator(self, mock_provider: MagicMock, tmp_path: Path):
        """自定义Validator."""
        mock_validator = MagicMock()
        mock_validator.validate.return_value.level = QualityLevel.PASS
        c = FinancialDataCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "fin"),
            validator=mock_validator,
        )
        c.collect(symbol="000001.SZ", report_type="income")
        mock_validator.validate.assert_called()

    # ── Parquet持久化 ─────────────────────────────────────────

    def test_parquet_persisted(self, collector: FinancialDataCollector):
        """采集后Parquet文件被写入."""
        collector.collect(symbol="000001.SZ", report_type="income")
        path = collector._parquet_path("000001.SZ", "income")
        assert path.exists()
        loaded = pd.read_parquet(path)
        assert len(loaded) > 0

    def test_parquet_separate_per_report_type(self, collector: FinancialDataCollector):
        """不同报表类型分别存储."""
        collector.collect_all(symbol="000001.SZ")
        for rt in ["income", "balance", "cashflow"]:
            path = collector._parquet_path("000001.SZ", rt)
            assert path.exists(), f"{rt} parquet not found"

    def test_restart_recovery(self, collector: FinancialDataCollector):
        """重启后进度恢复."""
        collector.collect(symbol="000001.SZ", report_type="income", end_date=date(2024, 12, 31))
        new_c = FinancialDataCollector(
            provider=collector.provider,
            storage_path=str(collector._storage_path),
        )
        assert new_c.get_progress("000001.SZ", "income") == date(2024, 12, 31)
