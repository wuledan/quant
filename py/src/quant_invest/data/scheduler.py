#!/usr/bin/env python3
"""数据调度服务 — 后台持续采集行情与新闻数据.

启动时全量拉取，之后按计划增量更新：
- 日线行情: 交易时段内每5分钟增量更新
- 新闻资讯: 每2分钟拉取最新
- 盘后全量: 每日16:00执行一次
"""

from __future__ import annotations

import asyncio
import json
import logging
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Any

import pandas as pd

from .collectors.daily_bar import DailyBarCollector
from .providers import DataProviderFactory
from .providers.base import AdjustMethod

logger = logging.getLogger("quant_invest.data.scheduler")

# 默认监控的标的列表
DEFAULT_SYMBOLS = [
    "000300.SH",   # 沪深300
    "000001.SH",   # 上证指数
    "399001.SZ",   # 深证成指
    "399006.SZ",   # 创业板指
    "600519.SH",   # 贵州茅台
    "300750.SZ",   # 宁德时代
]

# 数据本地存储根目录
DEFAULT_STORAGE_PATH = "./data"


class DataSchedulerService:
    """后台数据调度服务."""

    def __init__(
        self,
        symbols: list[str] | None = None,
        storage_path: str = DEFAULT_STORAGE_PATH,
    ) -> None:
        self._symbols = symbols or DEFAULT_SYMBOLS
        self._storage_path = Path(storage_path)
        self._storage_path.mkdir(parents=True, exist_ok=True)

        # 日线采集器
        self._provider = DataProviderFactory.create("yahoo")
        self._daily_collector = DailyBarCollector(
            provider=self._provider,
            storage_path=str(self._storage_path / "daily"),
        )

        # 新闻缓存
        self._news_cache: list[dict[str, Any]] = []
        self._news_last_update: datetime | None = None

        # 行情缓存 {symbol: DataFrame}
        self._daily_cache: dict[str, pd.DataFrame] = {}
        self._daily_cache_time: dict[str, datetime] = {}

        # 调度控制
        self._running = False
        self._tasks: list[asyncio.Task] = []

    # ── 公开接口 ──────────────────────────────────────────────

    def start(self) -> None:
        """启动调度."""
        if self._running:
            return
        self._running = True
        logger.info("数据调度服务启动, 监控标的: %s", self._symbols)

    def stop(self) -> None:
        """停止调度."""
        self._running = False
        for t in self._tasks:
            t.cancel()
        self._tasks.clear()
        logger.info("数据调度服务停止")

    async def run_forever(self) -> None:
        """主调度循环."""
        self.start()

        # 启动时立即执行一次全量采集
        await self._run_daily_update()
        await self._run_news_update()

        while self._running:
            try:
                now = datetime.now()

                # 判断是否在交易时段 (9:00-15:30 工作日)
                is_trading_hours = (
                    9 <= now.hour < 16
                    and now.weekday() < 5
                )

                if is_trading_hours:
                    # 交易时段: 高频更新
                    await asyncio.gather(
                        self._run_daily_incremental(),
                        self._run_news_update(),
                    )
                    await asyncio.sleep(120)  # 2分钟
                else:
                    # 非交易时段: 低频检查
                    # 16:00-16:30 执行盘后全量
                    if now.hour == 16 and now.minute < 30:
                        await self._run_daily_update()
                        await self._run_news_update()
                    await asyncio.sleep(600)  # 10分钟

            except asyncio.CancelledError:
                break
            except Exception as e:
                logger.error("调度循环异常: %s", e, exc_info=True)
                await asyncio.sleep(60)

    def get_daily_data(
        self,
        symbol: str,
        start_date: date | None = None,
        end_date: date | None = None,
    ) -> pd.DataFrame:
        """从本地缓存/Parquet获取日线数据."""
        # 1. 优先查内存缓存
        df = self._daily_cache.get(symbol)
        if df is not None and not df.empty:
            if start_date or end_date:
                mask = pd.Series(True, index=df.index)
                if start_date:
                    mask &= df.index >= pd.Timestamp(start_date)
                if end_date:
                    mask &= df.index <= pd.Timestamp(end_date)
                return df[mask]
            return df

        # 2. 从Parquet加载
        df = self._load_parquet(symbol)
        if df is not None and not df.empty:
            self._daily_cache[symbol] = df
            self._daily_cache_time[symbol] = datetime.now()
            if start_date or end_date:
                mask = pd.Series(True, index=df.index)
                if start_date:
                    mask &= df.index >= pd.Timestamp(start_date)
                if end_date:
                    mask &= df.index <= pd.Timestamp(end_date)
                return df[mask]
            return df

        # 3. 实时拉取
        return self._fetch_and_cache(symbol, start_date, end_date)

    def get_news(self, count: int = 50) -> list[dict[str, Any]]:
        """获取新闻缓存."""
        return self._news_cache[:count]

    def get_status(self) -> dict[str, Any]:
        """获取调度状态."""
        return {
            "running": self._running,
            "symbols": self._symbols,
            "daily_cache_count": len(self._daily_cache),
            "news_cache_count": len(self._news_cache),
            "news_last_update": self._news_last_update.isoformat() if self._news_last_update else None,
            "daily_cache_times": {
                s: t.isoformat() for s, t in self._daily_cache_time.items()
            },
        }

    # ── 采集任务 ──────────────────────────────────────────────

    async def _run_daily_update(self) -> None:
        """全量更新所有标的日线数据."""
        logger.info("开始全量日线数据更新, 标的数: %d", len(self._symbols))
        loop = asyncio.get_event_loop()

        for symbol in self._symbols:
            try:
                end_date = date.today()
                start_date = end_date - timedelta(days=365)  # 拉取1年数据

                df = await loop.run_in_executor(
                    None,
                    lambda s=symbol, sd=start_date, ed=end_date: self._daily_collector.collect(s, sd, ed),
                )
                if not df.empty:
                    self._daily_cache[symbol] = df
                    self._daily_cache_time[symbol] = datetime.now()
                    logger.info("日线数据更新: %s, %d条记录", symbol, len(df))
                else:
                    logger.warning("日线数据为空: %s", symbol)

            except Exception as e:
                logger.error("日线数据更新失败 %s: %s", symbol, e)

    async def _run_daily_incremental(self) -> None:
        """增量更新日线数据."""
        loop = asyncio.get_event_loop()

        for symbol in self._symbols:
            try:
                # 获取本地最新进度
                progress = self._daily_collector.get_progress(symbol)
                start_date = progress + timedelta(days=1) if progress else date.today() - timedelta(days=5)
                end_date = date.today()

                if start_date > end_date:
                    continue

                df = await loop.run_in_executor(
                    None,
                    lambda s=symbol, sd=start_date, ed=end_date: self._daily_collector.collect(s, sd, ed),
                )
                if not df.empty:
                    self._daily_cache[symbol] = df
                    self._daily_cache_time[symbol] = datetime.now()
                    logger.debug("增量更新: %s, 最新日期=%s", symbol, df.index[-1].strftime("%Y-%m-%d"))

            except Exception as e:
                logger.error("增量更新失败 %s: %s", symbol, e)

    async def _run_news_update(self) -> None:
        """更新新闻数据."""
        loop = asyncio.get_event_loop()
        try:
            from .providers.cls_news_provider import ClsNewsProvider
            provider = ClsNewsProvider()

            items = await loop.run_in_executor(None, lambda: provider.get_latest_news(count=100))
            self._news_cache = [
                {
                    "id": item.news_id,
                    "title": item.title,
                    "content": item.content[:200],
                    "source": item.source,
                    "published_at": item.published_at.isoformat(),
                    "symbols": item.symbols,
                    "tags": item.tags,
                    "importance": item.importance,
                }
                for item in items
            ]
            self._news_last_update = datetime.now()
            logger.info("新闻更新: %d条", len(self._news_cache))

        except Exception as e:
            logger.error("新闻更新失败: %s", e)

    # ── 内部工具 ──────────────────────────────────────────────

    def _load_parquet(self, symbol: str) -> pd.DataFrame | None:
        """从Parquet文件加载."""
        safe = symbol.replace(".", "_")
        path = self._storage_path / "daily" / f"{safe}.parquet"
        if not path.exists():
            return None
        try:
            return pd.read_parquet(path)
        except Exception as e:
            logger.error("读取Parquet失败 %s: %s", path, e)
            return None

    def _fetch_and_cache(
        self,
        symbol: str,
        start_date: date | None,
        end_date: date | None,
    ) -> pd.DataFrame:
        """实时拉取并缓存."""
        try:
            sd = start_date or date.today() - timedelta(days=365)
            ed = end_date or date.today()
            df = self._provider.get_daily_bars(symbol, sd, ed)
            if not df.empty:
                self._daily_cache[symbol] = df
                self._daily_cache_time[symbol] = datetime.now()
            return df
        except Exception as e:
            logger.error("实时拉取失败 %s: %s", symbol, e)
            return pd.DataFrame()


# ── 全局单例 ──────────────────────────────────────────────

_scheduler_instance: DataSchedulerService | None = None


def get_scheduler() -> DataSchedulerService:
    """获取全局调度器实例."""
    global _scheduler_instance
    if _scheduler_instance is None:
        _scheduler_instance = DataSchedulerService()
    return _scheduler_instance
