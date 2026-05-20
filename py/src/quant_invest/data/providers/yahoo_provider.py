#!/usr/bin/env python3
"""Yahoo Finance 数据源适配器 — 通过 yfinance 获取全球市场数据."""

from __future__ import annotations

import time
from datetime import date, datetime
from functools import wraps
from typing import Any, Callable

import pandas as pd

from .base import AdjustMethod, DataFreq, DataProvider


def retry(max_retries: int = 2, delay: float = 1.0) -> Callable:
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
                        wait *= 2
            raise RuntimeError(f"{func.__name__} failed after {max_retries} retries") from last_exc
        return wrapper
    return decorator


# A股 symbol 映射: yfinance 使用 .SS / .SZ 后缀
_A_SUFFIX_MAP = {
    ".SH": ".SS",
    ".SZ": ".SZ",
}


def _to_yahoo_symbol(symbol: str) -> str:
    """将本地 symbol 格式转为 yfinance 格式.

    本地格式: 000300.SH → yfinance: 000300.SS
    """
    s = symbol.upper()
    for cn_suffix, yahoo_suffix in _A_SUFFIX_MAP.items():
        if s.endswith(cn_suffix):
            return s[: -len(cn_suffix)] + yahoo_suffix
    return symbol


class YahooProvider(DataProvider):
    """Yahoo Finance 数据源适配器."""

    def __init__(self, config: dict | None = None) -> None:
        self._config = config or {}

    @property
    def name(self) -> str:
        return "yahoo"

    @retry(max_retries=2, delay=0.5)
    def get_daily_bars(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        import yfinance as yf

        yahoo_sym = _to_yahoo_symbol(symbol)
        ticker = yf.Ticker(yahoo_sym)
        hist = ticker.history(
            start=start_date.strftime("%Y-%m-%d"),
            end=end_date.strftime("%Y-%m-%d"),
        )

        if hist.empty:
            return hist

        # 标准化列名
        df = hist.rename(columns={
            "Open": "open",
            "High": "high",
            "Low": "low",
            "Close": "close",
            "Volume": "volume",
        })

        df.index.name = "trade_date"
        df.index = pd.to_datetime(df.index)

        required = {"open", "high", "low", "close", "volume"}
        missing = required - set(df.columns)
        if missing:
            raise ValueError(f"Missing required columns: {missing}")

        # yfinance 不直接提供成交额
        if "amount" not in df.columns:
            df["amount"] = df["volume"] * df["close"] * 1.0

        return df

    def get_minute_bars(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        raise NotImplementedError("YahooProvider 不支持分钟线")

    def get_tick_data(self, symbol: str, trade_date: date) -> pd.DataFrame:
        raise NotImplementedError("YahooProvider 不支持 tick 数据")

    def get_index_constituents(self, index_symbol: str, trade_date: date | None = None) -> pd.DataFrame:
        raise NotImplementedError("YahooProvider 不支持指数成分股")

    def get_trade_calendar(self, start_date: date, end_date: date) -> list[date]:
        """从 yfinance 数据推断交易日历."""
        import yfinance as yf

        ticker = yf.Ticker("000300.SS")
        hist = ticker.history(start=start_date.strftime("%Y-%m-%d"), end=end_date.strftime("%Y-%m-%d"))
        if hist.empty:
            return []
        return [d.date() for d in hist.index]

    def get_financial_data(self, symbol: str, report_type: str = 'income', start_date: date | None = None, end_date: date | None = None) -> pd.DataFrame:
        return pd.DataFrame()

    def get_industry_classification(self, symbol: str | None = None, level: str = 'L1') -> pd.DataFrame:
        return pd.DataFrame()

    def health_check(self) -> bool:
        try:
            import yfinance as yf
            ticker = yf.Ticker("000300.SS")
            hist = ticker.history(period="5d")
            return not hist.empty
        except Exception:
            return False
