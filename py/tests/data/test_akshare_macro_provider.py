#!/usr/bin/env python3
"""AkshareMacroProvider单元测试."""

from __future__ import annotations

from datetime import date
from unittest.mock import MagicMock, patch

import pandas as pd
import pytest

from quant_invest.data.providers.akshare_macro_provider import AkshareMacroProvider


class TestAkshareMacroProvider:
    """AkshareMacroProvider 单元测试."""

    @pytest.fixture
    def provider(self) -> AkshareMacroProvider:
        return AkshareMacroProvider()

    def test_name(self, provider):
        assert provider.name == "akshare_macro"

    @patch("akshare.macro_china_gdp")
    def test_get_china_gdp(self, mock_gdp, provider):
        mock_gdp.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [100.0], "change": [0.1]})
        result = provider.get_china_gdp(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty
        mock_gdp.assert_called_once()

    @patch("akshare.macro_china_cpi_yearly")
    def test_get_china_cpi(self, mock_cpi, provider):
        mock_cpi.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [100.5]})
        result = provider.get_china_cpi(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty
        mock_cpi.assert_called_once()

    @patch("akshare.macro_china_pmi")
    def test_get_china_pmi(self, mock_pmi, provider):
        mock_pmi.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [50.5]})
        result = provider.get_china_pmi(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.macro_china_money_supply")
    def test_get_china_money_supply(self, mock_ms, provider):
        mock_ms.return_value = pd.DataFrame({"date": ["2024-01-01"], "M2": [300_000]})
        result = provider.get_china_money_supply(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.macro_china_shrzgm")
    def test_social_financing(self, mock_sf, provider):
        mock_sf.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [5000]})
        result = provider.get_china_social_financing(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.rate_interbank")
    def test_interest_rate(self, mock_rate, provider):
        mock_rate.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [3.5]})
        result = provider.get_china_interest_rate(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.currency_boc_sina")
    def test_fx_rate(self, mock_fx, provider):
        mock_fx.return_value = pd.DataFrame({"date": ["2024-01-01"], "price": [7.12]})
        result = provider.get_fx_rate("USDCNY", date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.macro_usa_interest_rate")
    def test_us_fed_rate(self, mock_fed, provider):
        mock_fed.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [5.5]})
        result = provider.get_us_fed_rate(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.macro_usa_cpi_monthly")
    def test_us_cpi(self, mock_cpi, provider):
        mock_cpi.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [3.2]})
        result = provider.get_us_cpi(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.macro_usa_non_farm")
    def test_us_nonfarm(self, mock_nf, provider):
        mock_nf.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [200_000]})
        result = provider.get_us_nonfarm(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    @patch("akshare.stock_em_hsgt_north_net_flow_in_em")
    def test_northbound_flow(self, mock_nb, provider):
        mock_nb.return_value = pd.DataFrame({"date": ["2024-01-01"], "value": [5_000_000]})
        result = provider.get_northbound_flow(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty

    def test_health_check_true(self, provider):
        with patch("akshare.macro_china_gdp") as mock_gdp:
            mock_gdp.return_value = pd.DataFrame({"date": ["2024-01-01"]})
            assert provider.health_check()

    def test_health_check_false(self, provider):
        with patch("akshare.macro_china_gdp", side_effect=Exception("no data")):
            assert not provider.health_check()

    @patch("akshare.macro_china_gdp")
    def test_retry_then_success(self, mock_gdp, provider):
        mock_gdp.side_effect = [ConnectionError("timeout"), pd.DataFrame({"date": ["2024-01-01"]})]
        result = provider.get_china_gdp(date(2024, 1, 1), date(2024, 12, 31))
        assert not result.empty
        assert mock_gdp.call_count == 2

    @patch("akshare.macro_china_gdp")
    def test_empty_data(self, mock_gdp, provider):
        mock_gdp.return_value = pd.DataFrame()
        result = provider.get_china_gdp(date(2024, 1, 1), date(2024, 12, 31))
        assert result.empty
