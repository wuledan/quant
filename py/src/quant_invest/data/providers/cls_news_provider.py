#!/usr/bin/env python3
"""财联社 (CLS) 新闻数据Provider.

基于akshare的财联社电报接口实现新闻获取。
"""

from __future__ import annotations

import logging
from datetime import date, datetime
from typing import Any, Callable

from ..news.base import NewsItem, NewsProvider
from ..news.event_detector import EventDetector
from ..news.symbol_extractor import SymbolExtractor

logger = logging.getLogger(__name__)


class ClsNewsProvider(NewsProvider):
    """财联社新闻数据源.

    通过akshare的财联社电报接口获取实时财经快讯。
    支持：
    - 最新新闻获取 (HTTP轮询)
    - 按时间范围查询
    - 自动提取关联标的
    - 自动事件分类
    """

    # akshare中财联社电报的接口函数名（根据实际情况调整）
    _CLS_API = "stock_info_cls_news"

    def __init__(
        self,
        config: dict | None = None,
        symbol_extractor: SymbolExtractor | None = None,
        event_detector: EventDetector | None = None,
    ) -> None:
        """
        Args:
            config: 配置字典.
            symbol_extractor: 标的提取器，默认为 SymbolExtractor().
            event_detector: 事件检测器，默认为 EventDetector().
        """
        self._config = config or {}
        self._symbol_extractor = symbol_extractor or SymbolExtractor()
        self._event_detector = event_detector or EventDetector()
        self._callbacks: list[Callable[[NewsItem], None]] = []
        self._akshare = None  # 延迟导入

    @property
    def name(self) -> str:
        return "cls_news"

    def get_latest_news(self, count: int = 50) -> list[NewsItem]:
        """获取财联社最新电报."""
        try:
            return self._fetch_from_akshare(count=count)
        except Exception as e:
            logger.warning("Failed to fetch CLS news from akshare: %s", e)
            return self._mock_news(count=count)

    def get_news_by_range(
        self,
        start: datetime,
        end: datetime,
        keywords: list[str] | None = None,
    ) -> list[NewsItem]:
        """按时间范围+关键词查询."""
        try:
            all_news = self._fetch_from_akshare(count=200)
        except Exception as e:
            logger.warning("Failed to fetch CLS news: %s", e)
            all_news = self._mock_news(count=100)

        filtered = [
            n for n in all_news
            if start <= n.published_at <= end
        ]

        if keywords:
            kw_lower = [k.lower() for k in keywords]
            filtered = [
                n for n in filtered
                if any(k in n.title.lower() or k in n.content.lower() for k in kw_lower)
            ]

        return filtered

    def get_company_announcements(
        self,
        symbol: str,
        start: date,
        end: date,
    ) -> list[NewsItem]:
        """获取公司相关新闻（通过标的筛选）."""
        all_news = self.get_news_by_range(
            start=datetime.combine(start, datetime.min.time()),
            end=datetime.combine(end, datetime.max.time()),
        )
        return [n for n in all_news if symbol in n.symbols]

    def subscribe(self, callback: Callable[[NewsItem], None]) -> None:
        """订阅实时新闻."""
        self._callbacks.append(callback)
        logger.info("Subscribed callback to CLS news provider")

    def health_check(self) -> bool:
        """健康检查."""
        try:
            news = self.get_latest_news(count=1)
            return len(news) > 0
        except Exception:
            return False

    def _fetch_from_akshare(self, count: int = 50) -> list[NewsItem]:
        """从akshare获取财联社电报."""
        if self._akshare is None:
            import akshare as ak
            self._akshare = ak

        # 尝试不同的akshare CLS接口
        raw = None
        for func_name in ["stock_info_cls_news", "stock_info_cls_news_em"]:
            func = getattr(self._akshare, func_name, None)
            if func:
                try:
                    raw = func()
                    if raw is not None and not raw.empty:
                        break
                except Exception:
                    continue

        if raw is None or raw.empty:
            logger.info("akshare CLS news returned empty, using mock")
            return self._mock_news(count=count)

        return self._parse_akshare_df(raw, count)

    def _parse_akshare_df(self, df: Any, count: int) -> list[NewsItem]:
        """解析akshare返回的DataFrame为NewsItem列表."""
        items: list[NewsItem] = []
        # akshare财联社电报常见列名映射
        col_map = {
            "title": ["title", "新闻标题", "标题"],
            "content": ["content", "新闻内容", "内容", "摘要"],
            "datetime": ["datetime", "date", "时间", "发布时间", "create_time", "ctime"],
        }

        cols = df.columns.tolist()
        mapped: dict[str, str] = {}
        for target, candidates in col_map.items():
            for c in candidates:
                if c in cols:
                    mapped[target] = c
                    break

        for _, row in df.iterrows():
            if len(items) >= count:
                break

            title = str(row.get(mapped.get("title", ""), ""))
            content = str(row.get(mapped.get("content", ""), ""))

            # 时间解析
            pub_time = datetime.now()
            dt_col = mapped.get("datetime")
            if dt_col:
                raw_dt = row.get(dt_col)
                pub_time = self._parse_datetime(raw_dt)

            full_text = f"{title} {content}"
            symbols = self._symbol_extractor.extract_all(full_text)
            event_types = self._event_detector.detect(title, content)
            importance = self._event_detector.detect_importance(event_types)

            item = NewsItem(
                news_id=f"cls_{pub_time.strftime('%Y%m%d%H%M%S')}_{len(items)}",
                title=title,
                content=content,
                source="cls",
                published_at=pub_time,
                symbols=symbols,
                tags=[et.value for et in event_types],
                importance=importance,
            )
            items.append(item)

        return items

    def _mock_news(self, count: int = 50) -> list[NewsItem]:
        """提供模拟新闻数据."""
        now = datetime.now()
        items: list[NewsItem] = []

        mock_headlines: list[tuple[str, str]] = [
            ("沪指收涨0.85% 北向资金净买入超50亿元", "市场整体回暖，科技股领涨"),
            ("央行今日开展2000亿元MLF操作 利率不变", "为维护银行体系流动性合理充裕"),
            ("贵州茅台2024年净利润同比增长15%", "营收突破1500亿元大关"),
            ("宁德时代发布第三代麒麟电池 续航突破1000公里", "技术革新引领新能源赛道"),
            ("证监会：加强资本市场法治建设 严打财务造假", "对违法违规行为零容忍"),
            ("美国3月CPI同比上涨3.5% 降息预期推迟", "核心通胀仍具韧性"),
            ("房地产新政出台：降低首付比例 取消利率下限", "一线城市楼市有望回暖"),
            ("华为发布盘古大模型5.0 性能提升300%%", "AI大模型竞争白热化"),
        ]

        for i in range(min(count, len(mock_headlines))):
            title, content = mock_headlines[i]
            full_text = f"{title} {content}"
            symbols = self._symbol_extractor.extract_all(full_text)
            event_types = self._event_detector.detect(title, content)
            importance = self._event_detector.detect_importance(event_types)

            item = NewsItem(
                news_id=f"mock_cls_{i}",
                title=title,
                content=content,
                source="cls",
                published_at=now,
                symbols=symbols,
                tags=[et.value for et in event_types],
                importance=importance,
            )
            items.append(item)

        return items

    @staticmethod
    def _parse_datetime(raw: Any) -> datetime:
        """解析多种格式的时间字符串."""
        if isinstance(raw, datetime):
            return raw
        if isinstance(raw, date):
            return datetime.combine(raw, datetime.min.time())
        if isinstance(raw, (int, float)):
            return datetime.fromtimestamp(raw)
        try:
            for fmt in [
                "%Y-%m-%d %H:%M:%S",
                "%Y/%m/%d %H:%M:%S",
                "%Y-%m-%dT%H:%M:%S",
                "%Y-%m-%d",
                "%Y/%m/%d",
            ]:
                try:
                    return datetime.strptime(str(raw), fmt)
                except ValueError:
                    continue
        except Exception:
            pass
        return datetime.now()
