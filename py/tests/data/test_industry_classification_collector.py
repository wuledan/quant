#!/usr/bin/env python3
"""IndustryClassificationCollector单元测试."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import MagicMock

import pandas as pd
import pytest

from quant_invest.data.collectors.industry_classification import IndustryClassificationCollector


class TestIndustryClassificationCollector:
    """IndustryClassificationCollector 单元测试."""

    @pytest.fixture
    def mock_provider(self) -> MagicMock:
        provider = MagicMock()
        df = pd.DataFrame({
            "行业名称": ["银行", "房地产"],
            "行业代码": ["SW001", "SW002"],
            "成分股数量": [42, 130],
        })
        provider.get_industry_classification.return_value = df
        return provider

    @pytest.fixture
    def collector(self, mock_provider: MagicMock, tmp_path: Path) -> IndustryClassificationCollector:
        return IndustryClassificationCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "industry"),
            max_retries=2,
            retry_delay=0.01,
        )

    def test_collect_l1(self, collector: IndustryClassificationCollector):
        """采集L1行业分类."""
        result = collector.collect(level="L1")
        assert not result.empty
        assert len(result) == 2

    def test_collect_for_symbol(self, collector: IndustryClassificationCollector):
        """采集某只股票的行业分类."""
        result = collector.collect(level="L1", symbol="000001.SZ")
        assert not result.empty

    def test_collect_empty(self, collector: IndustryClassificationCollector, mock_provider: MagicMock):
        """空数据."""
        mock_provider.get_industry_classification.return_value = pd.DataFrame()
        result = collector.collect(level="L1")
        assert result.empty

    def test_collect_parquet_persisted(self, collector: IndustryClassificationCollector):
        """Parquet持久化."""
        collector.collect(level="L1")
        path = collector._parquet_path("L1", None)
        assert path.exists()

    def test_get_classification(self, mock_provider: MagicMock, tmp_path: Path):
        """获取某只股票行业分类."""
        provider_df = pd.DataFrame({
            "股票代码": ["000001.SZ"],
            "行业": ["银行"],
        })
        mock_provider.get_industry_classification.return_value = provider_df
        c = IndustryClassificationCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "industry"),
        )
        result = c.get_classification(symbol="000001.SZ")
        assert "行业" in result
        assert result["行业"] == "银行"

    def test_get_classification_empty(self, collector: IndustryClassificationCollector, mock_provider: MagicMock):
        """空数据返回空dict."""
        mock_provider.get_industry_classification.return_value = pd.DataFrame()
        result = collector.get_classification(symbol="999999.SZ")
        assert result == {}

    def test_progress(self, collector: IndustryClassificationCollector):
        """进度记录."""
        collector.collect(level="L1")
        assert collector.get_progress(level="L1") is not None

    def test_clear_progress(self, collector: IndustryClassificationCollector):
        """清除进度."""
        collector.collect(level="L1")
        collector.clear_progress(level="L1")
        assert collector.get_progress(level="L1") is None

    def test_l1_l2_independent(self, collector: IndustryClassificationCollector):
        """L1和L2独立存储."""
        collector.collect(level="L1")
        collector.collect(level="L2")
        assert collector._parquet_path("L1", None).exists()
        assert collector._parquet_path("L2", None).exists()

    def test_retry(self, mock_provider: MagicMock, tmp_path: Path):
        """重试后成功."""
        c = IndustryClassificationCollector(
            provider=mock_provider,
            storage_path=str(tmp_path / "industry"),
            max_retries=3,
            retry_delay=0.01,
        )
        df = pd.DataFrame({"行业名称": ["银行"]})
        mock_provider.get_industry_classification.side_effect = [
            ConnectionError("timeout"),
            ConnectionError("timeout"),
            df,
        ]
        result = c.collect(level="L1")
        assert not result.empty
