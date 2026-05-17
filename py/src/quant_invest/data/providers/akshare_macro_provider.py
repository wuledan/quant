#!/usr/bin/env python3
"""Akshare宏观&资金面数据Provider.

替换mock_df为真实akshare API调用，数据标准化，异常处理+重试。
"""

from __future__ import annotations

import time
from datetime import date, timedelta
from functools import wraps
from typing import Any, Callable

import pandas as pd

from ..macro.base import CapitalFlowProvider, MacroProvider


def retry(max_retries: int = 3, delay: float = 1.0, backoff: float = 2.0) -> Callable:
    """指数退避重试装饰器."""
    def decorator(func: Callable) -> Callable:
        @wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            last_exc = None
            wait = delay
            for attempt in range(max_retries):
                try:
                    return func(*args, **kwargs)
                except Exception as e:
                    last_exc = e
                    if attempt < max_retries - 1:
                        time.sleep(wait)
                        wait *= backoff
            raise RuntimeError(f"{func.__name__} failed after {max_retries} retries") from last_exc
        return wrapper
    return decorator


def _normalize_date_col(df: pd.DataFrame) -> pd.DataFrame:
    """统一日期列名为'date'并转为datetime."""
    if df.empty:
        return df
    df = df.copy()
    date_cols = ["日期", "date", "trade_date", "index", "报告期"]
    for col in df.columns:
        if col in date_cols:
            df = df.rename(columns={col: "date"})
            break
    if "date" in df.columns:
        df["date"] = pd.to_datetime(df["date"])
    return df


def _filter_by_date(df: pd.DataFrame, start: date, end: date) -> pd.DataFrame:
    """按日期范围过滤数据."""
    if df.empty or "date" not in df.columns:
        return df
    mask = (df["date"] >= pd.Timestamp(start)) & (df["date"] <= pd.Timestamp(end))
    return df[mask].reset_index(drop=True)


class AkshareMacroProvider(MacroProvider, CapitalFlowProvider):
    """基于akshare的宏观与资金面数据源."""

    def __init__(self, config: dict | None = None) -> None:
        self._config = config or {}

    @property
    def name(self) -> str:
        return "akshare_macro"

    # === 中国宏观 ===

    @retry(max_retries=2, delay=0.5)
    def get_china_gdp(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_china_gdp()
        df = _normalize_date_col(df)
        if not df.empty and "date" in df.columns:
            df = df.sort_values("date")
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_china_cpi(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_china_cpi_yearly()
        if df.empty:
            df = ak.macro_china_cpi_monthly()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_china_pmi(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_china_pmi()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_china_money_supply(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_china_money_supply()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_china_social_financing(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_china_shrzgm()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_china_interest_rate(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        try:
            df = ak.rate_interbank(market="Shibor", symbol="Shibor", indicator="隔夜")
        except Exception:
            df = ak.macro_china_lpr()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_fx_rate(self, pair: str, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.currency_boc_sina(symbol=pair)
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    # === 海外宏观 ===

    @retry(max_retries=2, delay=0.5)
    def get_us_fed_rate(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_usa_interest_rate()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_us_cpi(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_usa_cpi_monthly()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_us_nonfarm(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.macro_usa_non_farm()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_intl_commodity(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        symbol_map = {"黄金": "gold", "原油": "oil", "铜": "copper"}
        mapped = symbol_map.get(symbol, symbol)
        try:
            df = ak.futures_main_sina(symbol=mapped)
        except Exception:
            df = pd.DataFrame()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    # === 资金面 ===

    @retry(max_retries=2, delay=0.5)
    def get_northbound_flow(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.stock_em_hsgt_north_net_flow_in_em(symbol="沪股通")
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_margin_trading(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.stock_margin_detail_sse(date="20240105")
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_block_trade(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.stock_block_trade_em()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_etf_flow(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.fund_etf_spot_em()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    @retry(max_retries=2, delay=0.5)
    def get_pbc_operation(self, start: date, end: date) -> pd.DataFrame:
        import akshare as ak
        df = ak.bond_zh_cov()
        df = _normalize_date_col(df)
        return _filter_by_date(df, start, end)

    def health_check(self) -> bool:
        try:
            import akshare as ak
            df = ak.macro_china_gdp()
            return not df.empty
        except Exception:
            return False
