#!/usr/bin/env python3
"""新闻数据接口：NewsItem 数据结构 + NewsProvider 抽象基类."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import date, datetime
from typing import Callable


@dataclass
class NewsItem:
    """新闻条目."""

    news_id: str
    title: str
    content: str
    source: str  # cls / sina / eastmoney
    published_at: datetime
    symbols: list[str] = field(default_factory=list)  # 关联标的
    tags: list[str] = field(default_factory=list)  # 分类标签: policy/earnings/macro/geo/industry
    sentiment: float | None = None  # 情绪得分 [-1, 1]
    importance: float = 0.5  # 重要性 [0, 1]


class NewsProvider(ABC):
    """新闻与事件数据源抽象基类."""

    @property
    @abstractmethod
    def name(self) -> str:
        """数据源名称."""
        ...

    @abstractmethod
    def get_latest_news(self, count: int = 50) -> list[NewsItem]:
        """获取最新新闻列表."""
        ...

    @abstractmethod
    def get_news_by_range(
        self,
        start: datetime,
        end: datetime,
        keywords: list[str] | None = None,
    ) -> list[NewsItem]:
        """按时间范围+关键词查询新闻."""
        ...

    @abstractmethod
    def get_company_announcements(
        self,
        symbol: str,
        start: date,
        end: date,
    ) -> list[NewsItem]:
        """上市公司公告."""
        ...

    @abstractmethod
    def subscribe(self, callback: Callable[[NewsItem], None]) -> None:
        """订阅实时新闻推送（可选实现）."""
        ...

    @abstractmethod
    def health_check(self) -> bool:
        """数据源健康检查."""
        ...
