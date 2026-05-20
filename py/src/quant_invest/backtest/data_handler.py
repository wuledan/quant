#!/usr/bin/env python3
"""数据处理器基类与实现."""

from __future__ import annotations

from abc import ABC, abstractmethod
from datetime import date

import pandas as pd

from .events import MarketEvent


class DataHandler(ABC):
    """数据处理器基类"""

    @abstractmethod
    def next_bar(self) -> MarketEvent | None:
        """获取下一根K线"""
        ...

    @abstractmethod
    def current_bar(self, symbol: str) -> dict | None:
        """获取当前K线数据"""
        ...

    @abstractmethod
    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取历史N根K线"""
        ...


class DailyDataHandler(DataHandler):
    """日线数据处理器.

    优先从调度器缓存加载数据，缓存未命中时实时拉取。
    在初始化时预加载所有数据，按时间推进依次返回每根K线。
    """

    def __init__(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        data_provider: object,
    ) -> None:
        self._symbols = symbols
        self._data: dict[str, pd.DataFrame] = {}
        self._current_idx: int = 0
        self._dates: list[pd.Timestamp] = []
        self._load_data(start_date, end_date, data_provider)

    def _load_data(self, start_date: date, end_date: date, data_provider: object) -> None:
        """预加载所有数据 — 优先调度器缓存, 缓存不足时实时拉取."""
        # 1. 尝试从调度器缓存加载
        try:
            from ..data.scheduler import get_scheduler
            scheduler = get_scheduler()
            for symbol in self._symbols:
                df = scheduler.get_daily_data(symbol, start_date=start_date, end_date=end_date)
                if df is not None and not df.empty:
                    self._data[symbol] = df
        except Exception:
            pass

        # 2. 对缓存未命中或数据不足的标的实时拉取
        for symbol in self._symbols:
            cached = self._data.get(symbol)
            if cached is not None and not cached.empty:
                # 检查缓存是否覆盖了请求的日期范围
                if self._covers_range(cached, start_date, end_date):
                    continue
                # 缓存不足，补充拉取
                if hasattr(data_provider, 'get_daily_bars'):
                    try:
                        df = data_provider.get_daily_bars(symbol=symbol, start_date=start_date, end_date=end_date)
                        if not df.empty:
                            # 合并缓存和实时数据
                            combined = pd.concat([cached, df])
                            combined = combined[~combined.index.duplicated(keep='last')]
                            combined.sort_index(inplace=True)
                            self._data[symbol] = combined
                    except Exception:
                        pass
            else:
                # 完全无缓存，实时拉取
                if hasattr(data_provider, 'get_daily_bars'):
                    try:
                        df = data_provider.get_daily_bars(symbol=symbol, start_date=start_date, end_date=end_date)
                        if not df.empty:
                            self._data[symbol] = df
                    except Exception:
                        self._data[symbol] = pd.DataFrame()
                else:
                    self._data[symbol] = pd.DataFrame()

        # 3. 按请求日期范围裁剪（统一去掉时区避免比较报错）
        for symbol in self._symbols:
            df = self._data.get(symbol)
            if df is not None and not df.empty:
                # 去掉时区
                if isinstance(df.index, pd.DatetimeIndex) and df.index.tz is not None:
                    df.index = df.index.tz_localize(None)
                    self._data[symbol] = df
                mask = pd.Series(True, index=df.index)
                if start_date:
                    mask &= df.index >= pd.Timestamp(start_date)
                if end_date:
                    mask &= df.index <= pd.Timestamp(end_date)
                self._data[symbol] = df[mask]

        # 合并统一的时间索引
        all_dates: set[pd.Timestamp] = set()
        for df in self._data.values():
            if not df.empty:
                all_dates.update(df.index)

        self._dates = sorted(all_dates) if all_dates else []

    @staticmethod
    def _covers_range(df: pd.DataFrame, start: date, end: date) -> bool:
        """检查DataFrame是否覆盖了请求的日期范围."""
        if df.empty:
            return False
        df_start = df.index.min().date() if hasattr(df.index.min(), 'date') else df.index.min()
        df_end = df.index.max().date() if hasattr(df.index.max(), 'date') else df.index.max()
        if hasattr(df_start, 'year'):
            df_start = date(df_start.year, df_start.month, df_start.day)
        if hasattr(df_end, 'year'):
            df_end = date(df_end.year, df_end.month, df_end.day)
        return df_start <= start and df_end >= end

    def next_bar(self) -> MarketEvent | None:
        """获取下一根K线的市场事件."""
        if self._current_idx >= len(self._dates):
            return None

        current_date = self._dates[self._current_idx]
        bar_data: dict[str, dict] = {}

        for symbol in self._symbols:
            df = self._data.get(symbol, pd.DataFrame())
            if not df.empty and current_date in df.index:
                row = df.loc[current_date]
                bar_data[symbol] = row.to_dict()

        event = MarketEvent(
            timestamp=current_date.to_pydatetime(),
            symbol=",".join(self._symbols),
            bar_data=bar_data,
        )

        self._current_idx += 1
        return event

    def current_bar(self, symbol: str) -> dict | None:
        """获取当前K线数据."""
        if self._current_idx == 0:
            return None
        idx = self._current_idx - 1
        if idx >= len(self._dates):
            return None
        current_date = self._dates[idx]
        df = self._data.get(symbol, pd.DataFrame())
        if df.empty or current_date not in df.index:
            return None
        return df.loc[current_date].to_dict()

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取历史N根K线."""
        df = self._data.get(symbol, pd.DataFrame())
        if df.empty or self._current_idx == 0:
            return pd.DataFrame()

        end_idx = min(self._current_idx, len(self._dates))
        start_idx = max(0, end_idx - bars)
        relevant_dates = self._dates[start_idx:end_idx]
        return df[df.index.isin(relevant_dates)]


class MinuteDataHandler(DataHandler):
    """分钟线数据处理器.

    与日线不同，分钟线数据量较大，采用流式加载：
    - 启动时仅加载元数据和索引
    - 按需从Parquet文件流式读取
    - 内存中仅保留滑动窗口数据
    """

    def __init__(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        data_provider: object,
        window: int = 200,
    ) -> None:
        self._symbols = symbols
        self._window = window
        self._buffer: dict[str, pd.DataFrame] = {}
        self._dates: list[pd.Timestamp] = []
        self._current_idx: int = 0

    def next_bar(self) -> MarketEvent | None:
        raise NotImplementedError("MinuteDataHandler not yet implemented")

    def current_bar(self, symbol: str) -> dict | None:
        raise NotImplementedError("MinuteDataHandler not yet implemented")

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        raise NotImplementedError("MinuteDataHandler not yet implemented")


class StorageEngineDataHandler(DataHandler):
    """基于 C++ StorageEngine 的日线数据处理器.

    直接从 StorageEngineAdapter 查询数据，不经过 DataSchedulerService。
    适用于需要低延迟数据访问的回测场景。
    """

    def __init__(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        storage_adapter: object | None = None,
        data_dir: str = "py/data/daily",
    ) -> None:
        self._symbols = symbols
        self._data: dict[str, pd.DataFrame] = {}
        self._current_idx: int = 0
        self._dates: list[pd.Timestamp] = []
        self._storage_adapter = storage_adapter

        if storage_adapter is None:
            from ..storage.adapter import StorageEngineAdapter
            self._storage_adapter = StorageEngineAdapter(data_dir=data_dir)

        self._load_data(start_date, end_date)

    def _load_data(self, start_date: date, end_date: date) -> None:
        """从 StorageEngine 加载数据."""
        adapter = self._storage_adapter

        # 预加载所有 Parquet
        adapter.load_all_cached()

        for symbol in self._symbols:
            df = adapter.query_kline_as_df(symbol)
            if df is not None and not df.empty:
                # 去掉时区
                if isinstance(df.index, pd.DatetimeIndex) and df.index.tz is not None:
                    df.index = df.index.tz_localize(None)
                # 按日期范围裁剪
                mask = pd.Series(True, index=df.index)
                if start_date:
                    mask &= df.index >= pd.Timestamp(start_date)
                if end_date:
                    mask &= df.index <= pd.Timestamp(end_date)
                self._data[symbol] = df[mask]

        # 合并统一的时间索引
        all_dates: set[pd.Timestamp] = set()
        for df in self._data.values():
            if not df.empty:
                all_dates.update(df.index)
        self._dates = sorted(all_dates) if all_dates else []

    def next_bar(self) -> MarketEvent | None:
        """获取下一根K线的市场事件."""
        if self._current_idx >= len(self._dates):
            return None

        current_date = self._dates[self._current_idx]
        bar_data: dict[str, dict] = {}

        for symbol in self._symbols:
            df = self._data.get(symbol, pd.DataFrame())
            if not df.empty and current_date in df.index:
                row = df.loc[current_date]
                bar_data[symbol] = row.to_dict()

        event = MarketEvent(
            timestamp=current_date.to_pydatetime(),
            symbol=",".join(self._symbols),
            bar_data=bar_data,
        )

        self._current_idx += 1
        return event

    def current_bar(self, symbol: str) -> dict | None:
        """获取当前K线数据."""
        if self._current_idx == 0:
            return None
        idx = self._current_idx - 1
        if idx >= len(self._dates):
            return None
        current_date = self._dates[idx]
        df = self._data.get(symbol, pd.DataFrame())
        if df.empty or current_date not in df.index:
            return None
        return df.loc[current_date].to_dict()

    def history(self, symbol: str, bars: int = 20) -> pd.DataFrame:
        """获取历史N根K线."""
        df = self._data.get(symbol, pd.DataFrame())
        if df.empty or self._current_idx == 0:
            return pd.DataFrame()

        end_idx = min(self._current_idx, len(self._dates))
        start_idx = max(0, end_idx - bars)
        relevant_dates = self._dates[start_idx:end_idx]
        return df[df.index.isin(relevant_dates)]
