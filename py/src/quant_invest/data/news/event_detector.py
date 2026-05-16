#!/usr/bin/env python3
"""事件检测与分类：将新闻归类到主题事件."""

from __future__ import annotations

from enum import Enum


class EventType(str, Enum):
    """事件类型枚举."""

    POLICY = "policy"  # 政策/监管
    EARNINGS = "earnings"  # 财报/业绩
    MACRO = "macro"  # 宏观经济
    GEO = "geo"  # 地缘政治
    INDUSTRY = "industry"  # 行业动态
    COMPANY = "company"  # 公司事件
    MARKET = "market"  # 市场整体
    TECH = "tech"  # 技术突破
    OTHER = "other"  # 其他


# 事件类型→关键词映射
_EVENT_KEYWORDS: dict[EventType, list[str]] = {
    EventType.POLICY: [
        "政策", "监管", "证监会", "央行", "降准", "降息", "加息",
        "LPR", "利率", "存款准备金", "逆回购", "MLF", "国常会",
        "政治局", "国务院", "发改委", "财政部", "税务总局",
    ],
    EventType.EARNINGS: [
        "财报", "营收", "净利润", "业绩预告", "快报", "EPS",
        "每股收益", "同比增长", "环比增长", "扭亏", "预增", "预亏",
    ],
    EventType.MACRO: [
        "GDP", "CPI", "PPI", "PMI", "社融", "M2", "M1", "M0",
        "出口", "进口", "贸易顺差", "外汇储备", "通胀", "通缩",
        "非农", "失业率", "消费者信心",
    ],
    EventType.GEO: [
        "地缘", "冲突", "战争", "制裁", "关税", "贸易战",
        "中美", "俄乌", "中东", "欧盟", "北约", "联合国",
        "外交", "大使", "声明", "抗议",
    ],
    EventType.INDUSTRY: [
        "产业链", "供应链", "涨价", "降价", "产能", "供需",
        "新能源", "光伏", "锂电", "芯片", "半导体", "AI",
        "人工智能", "大模型", "医药", "集采", "房地产", "基建",
    ],
    EventType.COMPANY: [
        "涨停", "跌停", "停牌", "复牌", "增持", "减持", "回购",
        "分红", "送转", "配股", "定增", "并购", "重组", "借壳",
        "退市", "ST", "立案", "调查", "处罚",
    ],
    EventType.MARKET: [
        "大盘", "沪指", "深成指", "创业板指", "科创板",
        "北向资金", "主力资金", "成交量", "A股", "港股",
        "美股", "牛市", "熊市", "震荡", "反弹",
    ],
    EventType.TECH: [
        "技术突破", "研发", "专利", "创新", "首发",
        "芯片制程", "光刻机", "量子计算", "无人驾驶",
    ],
}


class EventDetector:
    """事件检测与分类器."""

    def __init__(self, custom_keywords: dict[EventType, list[str]] | None = None) -> None:
        """
        Args:
            custom_keywords: 自定义关键词覆盖.
        """
        self._keywords: dict[EventType, list[str]] = {}
        for event_type, kws in _EVENT_KEYWORDS.items():
            self._keywords[event_type] = list(kws)
        if custom_keywords:
            for event_type, kws in custom_keywords.items():
                if event_type in self._keywords:
                    self._keywords[event_type].extend(kws)
                else:
                    self._keywords[event_type] = list(kws)

    def detect(self, title: str, content: str = "") -> list[EventType]:
        """检测新闻文本所属的事件类型（可多标签）.

        Args:
            title: 新闻标题.
            content: 新闻正文.

        Returns:
            事件类型列表，按匹配度降序排列.
        """
        text = f"{title} {content}".lower()

        scores: dict[EventType, int] = {}
        for event_type, kws in self._keywords.items():
            score = sum(1 for kw in kws if kw.lower() in text)
            if score > 0:
                scores[event_type] = score

        if not scores:
            return [EventType.OTHER]

        # 按匹配度降序
        sorted_types = sorted(scores.keys(), key=lambda t: scores[t], reverse=True)
        return sorted_types

    def detect_importance(self, event_types: list[EventType]) -> float:
        """估算事件重要性 [0, 1].

        按事件类型给出基准重要性:
        - 地缘政治/宏观/政策: 0.8-1.0 (高)
        - 公司事件/市场: 0.5-0.8 (中)
        - 行业/技术: 0.3-0.6 (中低)
        - 其他: 0.1-0.3 (低)
        """
        if not event_types:
            return 0.3

        base_importance: dict[EventType, float] = {
            EventType.GEO: 0.9,
            EventType.MACRO: 0.85,
            EventType.POLICY: 0.8,
            EventType.EARNINGS: 0.7,
            EventType.COMPANY: 0.65,
            EventType.MARKET: 0.6,
            EventType.INDUSTRY: 0.5,
            EventType.TECH: 0.4,
            EventType.OTHER: 0.2,
        }
        # 取最高重要性的事件类型
        top_type = event_types[0]
        return base_importance.get(top_type, 0.3)

    def add_keywords(self, event_type: EventType, keywords: list[str]) -> None:
        """为指定事件类型添加自定义关键词."""
        if event_type in self._keywords:
            self._keywords[event_type].extend(keywords)
        else:
            self._keywords[event_type] = list(keywords)
