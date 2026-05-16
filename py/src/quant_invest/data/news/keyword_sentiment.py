#!/usr/bin/env python3
"""基于关键词+金融词典的情绪分析（规则方法 Phase 1）."""

from __future__ import annotations

from datetime import date, datetime

import pandas as pd

from .base import NewsItem
from .sentiment import SentimentAnalyzer, SentimentResult


# 金融领域正面词典
_POSITIVE_WORDS: list[str] = [
    # 业绩/财务
    "增长", "大涨", "飙升", "盈利", "扭亏", "预增", "超预期",
    "创新高", "突破", "放量", "强势", "利好", "提振", "回升",
    "反弹", "复苏", "繁荣", "景气", "扩张", "加速",
    # 政策/信心
    "降准", "降息", "放水", "宽松", "支持", "扶持", "鼓励",
    "减税", "降费", "刺激", "利好政策", "放量",
    # 市场情绪
    "看涨", "买入", "推荐", "增持", "乐观", "信心", "机会",
    "低估值", "价值洼地", "黄金坑", "抄底",
    # 公司行为
    "回购", "分红", "送转", "增持", "中标", "合同签订",
]

# 金融领域负面词典
_NEGATIVE_WORDS: list[str] = [
    # 业绩/财务
    "下跌", "大跌", "暴跌", "亏损", "预亏", "爆雷", "不及预期",
    "新低", "破位", "缩量", "利空", "拖累", "下滑", "回落",
    "衰退", "萧条", "萎缩", "放缓", "恶化", "违约", "暴雷",
    # 政策/风险
    "加息", "收紧", "去杠杆", "处罚", "立案", "调查", "退市",
    "st", "风险提示", "监管", "整顿", "查封",
    # 市场情绪
    "看跌", "卖出", "减持", "悲观", "恐慌", "逃离", "踩踏",
    "泡沫", "崩盘", "失守", "危机",
    # 公司行为
    "减持", "质押", "冻结", "违约", "逾期", "仲裁", "诉讼",
]

# 程度副词及权重
_INTENSIFIERS: dict[str, float] = {
    "大幅": 1.5,
    "明显": 1.3,
    "显著": 1.3,
    "略有": 0.5,
    "小幅": 0.5,
    "较为": 0.8,
    "极度": 1.8,
    "严重": 1.8,
    "强烈": 1.5,
}

# 否定词（反转情感）
_NEGATORS: list[str] = [
    "不", "未", "没有", "并非", "并非", "并非",
    "无", "否", "不会", "未能", "尚未",
]


class KeywordSentimentAnalyzer(SentimentAnalyzer):
    """基于金融词典+规则的情绪分析器（Phase 1）.

    实现方法：
      1. 分词后匹配正面/负面词典
      2. 检查程度副词加权
      3. 检查否定词反转
      4. 归一化到 [-1, 1]

    可扩展自定义词典。
    """

    def __init__(
        self,
        positive_words: list[str] | None = None,
        negative_words: list[str] | None = None,
        intensifiers: dict[str, float] | None = None,
        negators: list[str] | None = None,
    ) -> None:
        self._positive = positive_words or _POSITIVE_WORDS
        self._negative = negative_words or _NEGATIVE_WORDS
        self._intensifiers = intensifiers or _INTENSIFIERS
        self._negators = negators or _NEGATORS

    def analyze_text(self, text: str) -> SentimentResult:
        """单条文本情绪分析."""
        score, magnitude, labels = self._analyze(text)
        return SentimentResult(score=score, magnitude=magnitude, labels=labels)

    def analyze_batch(self, items: list[NewsItem]) -> list[SentimentResult]:
        """批量情绪分析."""
        return [self.analyze_text(f"{item.title} {item.content}") for item in items]

    def get_sentiment_index(
        self,
        symbol: str,
        start: date,
        end: date,
    ) -> pd.DataFrame:
        """获取某标的的日级情绪指数.

        注意：此方法需要外部提供标的对应的新闻数据才能计算。
        这里返回空DataFrame，完整实现需注入 NewsProvider。
        """
        dates = pd.bdate_range(start, end)
        return pd.DataFrame({
            "date": dates,
            "sentiment": 0.0,
            "magnitude": 0.0,
            "news_count": 0,
        })

    def _analyze(self, text: str) -> tuple[float, float, list[str]]:
        """内部分析实现.

        Returns:
            (score, magnitude, labels)
        """
        pos_count = 0
        neg_count = 0
        total_weight = 0

        words = self._tokenize(text)

        i = 0
        while i < len(words):
            word = words[i]
            # 检查程度副词（前一个词）
            intensifier = 1.0
            if i > 0 and words[i - 1] in self._intensifiers:
                intensifier = self._intensifiers[words[i - 1]]

            # 检查否定词（前一个词，程度副词之前）
            negated = False
            if i > 0:
                check_idx = i - 1
                if words[check_idx] in self._negators:
                    negated = True
                elif (
                    check_idx > 0
                    and words[check_idx] in self._intensifiers
                    and words[check_idx - 1] in self._negators
                ):
                    negated = True

            # 匹配正面词
            if word in self._positive:
                weight = intensifier * (1.0 if not negated else -0.5)
                pos_count += weight
                total_weight += abs(weight)

            # 匹配负面词
            if word in self._negative:
                weight = intensifier * (1.0 if not negated else -0.5)
                neg_count += weight
                total_weight += abs(weight)

            i += 1

        # 归一化得分到 [-1, 1]
        if total_weight > 0:
            raw_score = (pos_count - neg_count) / total_weight
        else:
            raw_score = 0.0

        score = max(-1.0, min(1.0, raw_score))
        magnitude = min(1.0, total_weight / max(1, len(words)) * 2.0)

        # 标签判断
        labels = ["neutral"]
        if score > 0.2:
            labels = ["positive"]
        elif score < -0.2:
            labels = ["negative"]

        if magnitude > 0.6:
            if score > 0.3:
                labels.append("greed")
            elif score < -0.3:
                labels.append("fear")

        return score, magnitude, labels

    def _tokenize(self, text: str) -> list[str]:
        """简易中文分词（按字/词切分）.

        实际生产环境建议使用 jieba 分词。
        这里采用基于词典的最大正向匹配。
        """
        if not text:
            return []

        # 合并所有关键词（按长度降序，优先匹配长词）
        all_terms = list(
            set(
                self._positive
                + self._negative
                + list(self._intensifiers.keys())
                + self._negators
            )
        )
        all_terms.sort(key=len, reverse=True)

        tokens: list[str] = []
        remaining = text.lower()

        while remaining:
            matched = False
            for term in all_terms:
                if remaining.startswith(term):
                    tokens.append(term)
                    remaining = remaining[len(term):]
                    matched = True
                    break
            if not matched:
                # 单个字符
                tokens.append(remaining[0])
                remaining = remaining[1:]

        return tokens

    def add_positive_words(self, words: list[str]) -> None:
        """添加自定义正面词."""
        for w in words:
            if w not in self._positive:
                self._positive.append(w)

    def add_negative_words(self, words: list[str]) -> None:
        """添加自定义负面词."""
        for w in words:
            if w not in self._negative:
                self._negative.append(w)
