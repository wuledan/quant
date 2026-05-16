#!/usr/bin/env python3
"""新闻与舆情数据模块."""

from .base import NewsItem, NewsProvider
from .event_detector import EventDetector, EventType
from .keyword_sentiment import KeywordSentimentAnalyzer
from .sentiment import SentimentAnalyzer, SentimentResult
from .symbol_extractor import SymbolExtractor

__all__ = [
    "EventDetector",
    "EventType",
    "KeywordSentimentAnalyzer",
    "NewsItem",
    "NewsProvider",
    "SentimentAnalyzer",
    "SentimentResult",
    "SymbolExtractor",
]
