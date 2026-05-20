#!/usr/bin/env python3
"""AkShare数据源适配器.

使用akshare库获取A股数据，作为默认的免费数据源。
内置重试机制和频率限制，避免触发API限频。
"""

from __future__ import annotations

import time
from datetime import date, datetime
from functools import wraps
from typing import Any, Callable

import pandas as pd

from .base import AdjustMethod, DataFreq, DataProvider


def retry(max_retries: int = 3, delay: float = 1.0, backoff: float = 2.0) -> Callable:
    """重试装饰器：指数退避重试."""

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


class RateLimiter:
    """简易频率限制器."""

    def __init__(self, calls_per_second: float = 5.0) -> None:
        self._interval = 1.0 / calls_per_second
        self._last_call: float = 0.0

    def wait(self) -> None:
        """在需要时等待以保持频率限制."""
        elapsed = time.time() - self._last_call
        if elapsed < self._interval:
            time.sleep(self._interval - elapsed)
        self._last_call = time.time()


class AkshareProvider(DataProvider):
    """AkShare数据源适配器."""

    # 代理相关环境变量键
    _PROXY_KEYS = ("http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY")

    def __init__(self, config: dict | None = None) -> None:
        self._config = config or {}
        self._rate_limiter = RateLimiter(
            calls_per_second=self._config.get("calls_per_second", 5.0)
        )
        # 是否在请求时临时移除代理（国内数据源不需要走代理）
        self._bypass_proxy_enabled = self._config.get("bypass_proxy", True)

    def _without_proxy(self):
        """上下文管理器：临时清除代理环境变量，使 requests 直连."""
        import contextlib

        @contextlib.contextmanager
        def _ctx():
            if not self._bypass_proxy_enabled:
                yield
                return
            saved = {}
            import os
            for k in self._PROXY_KEYS:
                if k in os.environ:
                    saved[k] = os.environ.pop(k)
            try:
                yield
            finally:
                os.environ.update(saved)

        return _ctx()

    @property
    def name(self) -> str:
        return "akshare"

    @retry(max_retries=2, delay=0.5)
    def get_daily_bars(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        """获取日行情数据."""
        import akshare as ak

        adjust_map = {
            AdjustMethod.NONE: "",
            AdjustMethod.FORWARD: "qfq",
            AdjustMethod.BACKWARD: "hfq",
        }
        self._rate_limiter.wait()
        with self._without_proxy():
            try:
                df = ak.stock_zh_a_hist(
                    symbol=symbol,
                    period="daily",
                    start_date=start_date.strftime("%Y%m%d"),
                    end_date=end_date.strftime("%Y%m%d"),
                    adjust=adjust_map.get(adjust, "qfq"),
                )
            except Exception:
                df = ak.stock_zh_a_daily(
                    symbol=symbol,
                    start_date=start_date.strftime("%Y%m%d"),
                    end_date=end_date.strftime("%Y%m%d"),
                    adjust=adjust_map.get(adjust, "qfq"),
                )

        if df.empty:
            return df

        col_map = {
            "开盘": "open",
            "收盘": "close",
            "最高": "high",
            "最低": "low",
            "成交量": "volume",
            "成交额": "amount",
            "振幅": "amplitude",
            "涨跌幅": "pct_change",
            "涨跌额": "change",
            "换手率": "turnover",
        }
        df = df.rename(columns=col_map)

        date_cols = ["日期", "date", "trade_date"]
        for col in date_cols:
            if col in df.columns:
                df["trade_date"] = pd.to_datetime(df[col])
                df = df.drop(columns=[col])
                break

        df = df.set_index("trade_date")
        df.index.name = "trade_date"

        required = {"open", "high", "low", "close", "volume"}
        missing = required - set(df.columns)
        if missing:
            raise ValueError(f"Missing required columns: {missing}")

        return df

    def get_minute_bars(
        self,
        symbol: str,
        start_time: datetime,
        end_time: datetime,
        freq: DataFreq = DataFreq.MIN_1,
        adjust: AdjustMethod = AdjustMethod.NONE,
    ) -> pd.DataFrame:
        """获取分钟线数据."""
        import akshare as ak

        period_map = {
            DataFreq.MIN_1: "1",
            DataFreq.MIN_5: "5",
            DataFreq.MIN_15: "15",
            DataFreq.MIN_30: "30",
            DataFreq.MIN_60: "60",
        }
        period = period_map.get(freq, "1")

        self._rate_limiter.wait()
        df = ak.stock_zh_a_hist_min_em(
            symbol=symbol,
            period=period,
            start_date=start_time.strftime("%Y%m%d"),
            end_date=end_time.strftime("%Y%m%d"),
            adjust="" if adjust == AdjustMethod.NONE else "qfq",
        )

        if df.empty:
            return df

        col_map = {
            "时间": "time",
            "开盘": "open",
            "收盘": "close",
            "最高": "high",
            "最低": "low",
            "成交量": "volume",
            "成交额": "amount",
        }
        df = df.rename(columns=col_map)
        if "time" in df.columns:
            df = df.set_index(pd.to_datetime(df["time"]))
            df = df.drop(columns=["time"])

        return df

    def get_tick_data(
        self,
        symbol: str,
        trade_date: date,
    ) -> pd.DataFrame:
        """获取Tick级别数据."""
        import akshare as ak

        self._rate_limiter.wait()
        df = ak.stock_zh_a_tick_tx(
            symbol=symbol,
            start_date=trade_date.strftime("%Y%m%d"),
            end_date=trade_date.strftime("%Y%m%d"),
        )
        return df

    def get_financial_data(
        self,
        symbol: str,
        report_type: str = "income",
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> pd.DataFrame:
        """获取财务数据."""
        import akshare as ak

        self._rate_limiter.wait()
        type_map = {
            "income": ak.stock_profit_sheet_by_report_em,
            "balance": ak.stock_balance_sheet_by_report_em,
            "cashflow": ak.stock_cash_flow_sheet_by_report_em,
        }
        func = type_map.get(report_type)
        if func is None:
            raise ValueError(f"Unknown report_type: {report_type}")

        df = func(symbol=symbol)

        if not df.empty and start_date and "报告期" in df.columns:
            df = df[pd.to_datetime(df["报告期"]) >= pd.Timestamp(start_date)]
        if not df.empty and end_date and "报告期" in df.columns:
            df = df[pd.to_datetime(df["报告期"]) <= pd.Timestamp(end_date)]

        return df

    def get_index_constituents(
        self,
        index_symbol: str,
        trade_date: date | None = None,
    ) -> pd.DataFrame:
        """获取指数成分股."""
        import akshare as ak

        index_map = {
            "000300.SH": "000300",
            "000905.SH": "000905",
            "000016.SH": "000016",
            "000688.SH": "000688",
        }
        index_code = index_map.get(
            index_symbol,
            index_symbol.replace(".SH", "").replace(".SZ", ""),
        )

        self._rate_limiter.wait()
        df = ak.index_stock_cons(index_code)
        return df

    def get_industry_classification(
        self,
        symbol: str | None = None,
        level: str = "L1",
    ) -> pd.DataFrame:
        """获取行业分类."""
        import akshare as ak

        self._rate_limiter.wait()
        if symbol:
            df = ak.stock_board_industry_name_em()
            try:
                stock_info = ak.stock_individual_info_em(symbol=symbol)
                return stock_info
            except Exception:
                return pd.DataFrame()

        df = ak.stock_board_industry_name_em()
        return df

    def get_trade_calendar(
        self,
        start_date: date,
        end_date: date,
    ) -> list[date]:
        """获取交易日历."""
        import akshare as ak

        self._rate_limiter.wait()
        df = ak.tool_trade_date_hist_sina()
        if df.empty:
            return []

        date_col = "交易日历" if "交易日历" in df.columns else "trade_date"
        if date_col not in df.columns:
            # 尝试其他常见列名
            date_col = [c for c in ["date", "日期", "trade_date"] if c in df.columns]
            if not date_col:
                return []
            date_col = date_col[0]

        df[date_col] = pd.to_datetime(df[date_col])
        mask = (df[date_col] >= pd.Timestamp(start_date)) & (
            df[date_col] <= pd.Timestamp(end_date)
        )
        return df[mask][date_col].dt.date.tolist()

    def health_check(self) -> bool:
        """数据源健康检查."""
        try:
            import akshare as ak

            self._rate_limiter.wait()
            df = ak.tool_trade_date_hist_sina()
            return not df.empty
        except Exception:
            return False
