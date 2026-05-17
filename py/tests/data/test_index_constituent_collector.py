#!/usr/bin/env python3
"""IndexConstituentCollector单元测试."""

from __future__ import annotations

from datetime import date
from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.data.collectors.index_constituent import IndexConstituentCollector


class TestIndexConstituentCollector:
    """IndexConstituentCollector 单元测试."""

    @pytest.fixture
    def mock_provider(self) -> MagicMock:
        provider = MagicMock()
        df = pd.DataFrame({
            "constituent_code": ["000001.SZ", "600000.SH", "000002.SZ"],
            "name": ["平安银行", "浦发银行", "万科A"],
            "weight": [0.05, 0.03, 0.02],
        })
        provider.get_index_constituents.return_value = df
        return provider

    @pytest.fixture
    def collector(self, mock_provider: MagicMock, tmp_path: Path) -> IndexConstituentCollector:
        return IndexConstituentCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "index"),
            max_retries=2,
            retry_delay=0.01,
        )

    def test_collect(self, collector: IndexConstituentCollector):
        """采集成分股."""
        result = collector.collect(index_symbol="000300.SH")
        assert not result.empty
        assert len(result) == 3

    def test_collect_empty(self, collector: IndexConstituentCollector, mock_provider: MagicMock):
        """空数据."""
        mock_provider.get_index_constituents.return_value = pd.DataFrame()
        result = collector.collect(index_symbol="000300.SH")
        assert result.empty

    def test_collect_parquet_persisted(self, collector: IndexConstituentCollector):
        """Parquet持久化."""
        collector.collect(index_symbol="000300.SH", trade_date=date(2024, 6, 1))
        path = collector._parquet_path("000300.SH")
        assert path.exists()
        loaded = pd.read_parquet(path)
        assert len(loaded) == 3

    def test_diff_added_removed(self, mock_provider: MagicMock, tmp_path: Path):
        """成分股变动检测."""
        df_a = pd.DataFrame({"constituent_code": ["000001.SZ", "600000.SH"]})
        df_b = pd.DataFrame({"constituent_code": ["000001.SZ", "000002.SZ"]})
        mock_provider.get_index_constituents.side_effect = [df_a, df_b]

        c = IndexConstituentCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "index"),
        )
        result = c.diff("000300.SH", date(2024, 1, 1), date(2024, 6, 1))
        assert "000002.SZ" in result["added"]
        assert "600000.SH" in result["removed"]

    def test_diff_no_change(self, mock_provider: MagicMock, tmp_path: Path):
        """无变动."""
        df = pd.DataFrame({"constituent_code": ["000001.SZ", "600000.SH"]})
        mock_provider.get_index_constituents.return_value = df

        c = IndexConstituentCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "index"),
        )
        result = c.diff("000300.SH", date(2024, 1, 1), date(2024, 6, 1))
        assert result["added"] == []
        assert result["removed"] == []

    def test_diff_empty(self, mock_provider: MagicMock, tmp_path: Path):
        """空数据diff."""
        mock_provider.get_index_constituents.return_value = pd.DataFrame()
        c = IndexConstituentCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "index"),
        )
        result = c.diff("000300.SH", date(2024, 1, 1), date(2024, 6, 1))
        assert result["added"] == []
        assert result["removed"] == []

    def test_progress(self, collector: IndexConstituentCollector):
        """进度持久化."""
        collector.collect(index_symbol="000300.SH", trade_date=date(2024, 6, 1))
        assert collector.get_progress("000300.SH") == date(2024, 6, 1)

    def test_progress_none(self, collector: IndexConstituentCollector):
        """无进度."""
        assert collector.get_progress("999999.SH") is None

    def test_clear_progress(self, collector: IndexConstituentCollector):
        """清除进度."""
        collector.collect(index_symbol="000300.SH", trade_date=date(2024, 6, 1))
        collector.clear_progress("000300.SH")
        assert collector.get_progress("000300.SH") is None

    def test_retry(self, mock_provider: MagicMock, tmp_path: Path):
        """重试后成功."""
        c = IndexConstituentCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "index"),
            max_retries=3,
            retry_delay=0.01,
        )
        df = pd.DataFrame({"constituent_code": ["000001.SZ"]})
        mock_provider.get_index_constituents.side_effect = [
            ConnectionError("timeout"),
            ConnectionError("timeout"),
            df,
        ]
        result = c.collect(index_symbol="000300.SH")
        assert not result.empty
        assert mock_provider.get_index_constituents.call_count == 3

    def test_multiple_indices(self, collector: IndexConstituentCollector):
        """多指数独立存储."""
        for idx in ["000300.SH", "000905.SH"]:
            collector.collect(index_symbol=idx, trade_date=date(2024, 6, 1))
        assert collector._parquet_path("000300.SH").exists()
        assert collector._parquet_path("000905.SH").exists()
