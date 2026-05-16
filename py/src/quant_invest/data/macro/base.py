#!/usr/bin/env python3
"""宏观数据与资金面数据接口."""

from __future__ import annotations

from abc import ABC, abstractmethod
from datetime import date

import pandas as pd


class MacroProvider(ABC):
    """宏观经济数据源接口."""

    @property
    @abstractmethod
    def name(self) -> str: ...

    # === 中国宏观 ===

    @abstractmethod
    def get_china_gdp(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_cpi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_pmi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_money_supply(self, start: date, end: date) -> pd.DataFrame:
        """M0/M1/M2."""
        ...

    @abstractmethod
    def get_china_social_financing(self, start: date, end: date) -> pd.DataFrame:
        """社会融资规模."""
        ...

    @abstractmethod
    def get_china_interest_rate(self, start: date, end: date) -> pd.DataFrame:
        """Shibor/LPR/国债收益率."""
        ...

    @abstractmethod
    def get_fx_rate(self, pair: str, start: date, end: date) -> pd.DataFrame:
        """汇率: USD/CNY, EUR/CNY 等."""
        ...

    # === 海外宏观 ===

    @abstractmethod
    def get_us_fed_rate(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_us_cpi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_us_nonfarm(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_intl_commodity(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """国际商品: 原油/黄金/铜/大豆."""
        ...

    @abstractmethod
    def health_check(self) -> bool: ...


class CapitalFlowProvider(ABC):
    """资金面数据源接口."""

    @property
    @abstractmethod
    def name(self) -> str: ...

    @abstractmethod
    def get_northbound_flow(self, start: date, end: date) -> pd.DataFrame:
        """北向资金日流向."""
        ...

    @abstractmethod
    def get_margin_trading(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """融资融券数据."""
        ...

    @abstractmethod
    def get_block_trade(self, start: date, end: date) -> pd.DataFrame:
        """大宗交易."""
        ...

    @abstractmethod
    def get_etf_flow(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """ETF申赎资金流."""
        ...

    @abstractmethod
    def get_pbc_operation(self, start: date, end: date) -> pd.DataFrame:
        """央行公开市场操作."""
        ...

    @abstractmethod
    def health_check(self) -> bool: ...
