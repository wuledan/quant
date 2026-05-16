#!/usr/bin/env python3
"""舆情分析接口：SentimentAnalyzer 抽象基类 + SentimentResult."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from datetime import date

import pandas as pd

from .base import NewsItem


@dataclass
class SentimentResult:
    """情绪分析结果."""

    score: float  # [-1, 1], 负面→正面
    magnitude: float  # [0, 1], 情绪强度
    labels: list[str] = field(default_factory=lambda: ["neutral"])  # 情绪标签


class SentimentAnalyzer(ABC):
    """舆情分析器抽象基类.

    实现方案递进：
      Phase 1: Rule-based（关键词+金融词典）
      Phase 2: FinBERT（金融领域BERT微调）
      Phase 3: LLM API（GPT-4/Claude）
    """

    @abstractmethod
    def analyze_text(self, text: str) -> SentimentResult:
        """单条文本情绪分析."""
        ...

    @abstractmethod
    def analyze_batch(self, items: list[NewsItem]) -> list[SentimentResult]:
        """批量情绪分析."""
        ...

    @abstractmethod
    def get_sentiment_index(
        self,
        symbol: str,
        start: date,
        end: date,
    ) -> pd.DataFrame:
        """获取某标的的日级情绪指数."""
        ...
