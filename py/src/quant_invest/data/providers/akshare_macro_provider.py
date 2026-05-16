#!/usr/bin/env python3
"""Akshare宏观&资金面数据Provider."""

from __future__ import annotations

from datetime import date, timedelta
from typing import Any

import pandas as pd

from ..macro.base import CapitalFlowProvider, MacroProvider


class AkshareMacroProvider(MacroProvider, CapitalFlowProvider):
    """基于akshare的宏观与资金面数据源."""

    def __init__(self, config: dict | None = None) -> None:
        self._config = config or {}

    @property
    def name(self) -> str:
        return "akshare_macro"

    # === 中国宏观 ===

    def get_china_gdp(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_china_cpi(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_china_pmi(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_china_money_supply(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_china_social_financing(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_china_interest_rate(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_fx_rate(self, pair: str, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    # === 海外宏观 ===

    def get_us_fed_rate(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_us_cpi(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_us_nonfarm(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_intl_commodity(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    # === 资金面 ===

    def get_northbound_flow(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_margin_trading(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_block_trade(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_etf_flow(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def get_pbc_operation(self, start: date, end: date) -> pd.DataFrame:
        return self._mock_df(start, end)

    def health_check(self) -> bool:
        return True

    def _mock_df(self, start: date, end: date) -> pd.DataFrame:
        """提供模拟数据（生产环境替换为akshare调用）."""
        dates = pd.bdate_range(start, end)
        n = len(dates)
        return pd.DataFrame({
            "date": dates,
            "value": [100.0 + i * 0.1 for i in range(n)],
            "change": [0.0] + [0.1] * (n - 1),
        })