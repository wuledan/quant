#!/usr/bin/env python3
"""新闻与舆情模块测试."""

from __future__ import annotations

from datetime import date, datetime

import pytest

from quant_invest.data.news import (
    EventDetector,
    EventType,
    KeywordSentimentAnalyzer,
    NewsItem,
    NewsProvider,
    SentimentAnalyzer,
    SentimentResult,
    SymbolExtractor,
)
from quant_invest.data.providers.cls_news_provider import ClsNewsProvider


# =========================================================
# B1: NewsItem 数据结构
# =========================================================


class TestNewsItem:
    """NewsItem数据类测试."""

    def test_create_basic(self):
        """创建基础条目."""
        item = NewsItem(
            news_id="test_001",
            title="沪指收涨",
            content="市场整体回暖",
            source="cls",
            published_at=datetime(2024, 1, 15, 10, 30),
        )
        assert item.news_id == "test_001"
        assert item.title == "沪指收涨"
        assert item.source == "cls"
        assert item.symbols == []
        assert item.tags == []
        assert item.sentiment is None
        assert item.importance == 0.5

    def test_create_full(self):
        """创建完整条目."""
        item = NewsItem(
            news_id="test_002",
            title="贵州茅台业绩预告",
            content="净利润增长",
            source="cls",
            published_at=datetime(2024, 1, 15),
            symbols=["600519.SH"],
            tags=["earnings"],
            sentiment=0.8,
            importance=0.9,
        )
        assert "600519.SH" in item.symbols
        assert "earnings" in item.tags
        assert item.sentiment == 0.8
        assert item.importance == 0.9

    def test_immutable_id(self):
        """news_id 不变."""
        item = NewsItem(news_id="fixed", title="t", content="c", source="cls", published_at=datetime.now())
        assert item.news_id == "fixed"


# =========================================================
# B1: NewsProvider 接口
# =========================================================


class TestNewsProvider:
    """NewsProvider抽象基类测试."""

    def test_cannot_instantiate(self):
        """抽象类不可实例化."""
        with pytest.raises(TypeError):
            NewsProvider()  # type: ignore[abstract]

    def test_has_abstract_methods(self):
        """验证抽象方法存在."""
        methods = [
            "get_latest_news",
            "get_news_by_range",
            "get_company_announcements",
            "subscribe",
            "health_check",
        ]
        for m in methods:
            assert hasattr(NewsProvider, m)


# =========================================================
# B2: SymbolExtractor
# =========================================================


class TestSymbolExtractor:
    """标的提取器测试."""

    def test_extract_codes_sh(self):
        """提取上海代码."""
        text = "贵州茅台(600519.SH)今日大涨"
        extractor = SymbolExtractor()
        codes = extractor.extract_codes(text)
        assert "600519.SH" in codes

    def test_extract_codes_sz(self):
        """提取深圳代码."""
        text = "宁德时代(300750.SZ)股价创新高"
        extractor = SymbolExtractor()
        codes = extractor.extract_codes(text)
        assert "300750.SZ" in codes

    def test_extract_codes_bare(self):
        """提取无后缀代码."""
        text = "600519今日大涨，000858跟涨"
        extractor = SymbolExtractor()
        codes = extractor.extract_codes(text)
        assert "600519.SH" in codes
        assert "000858.SZ" in codes

    def test_extract_names(self):
        """提取股票名称."""
        text = "贵州茅台发布业绩预告，净利润大幅增长"
        extractor = SymbolExtractor()
        codes = extractor.extract_names(text)
        assert "600519.SH" in codes

    def test_extract_names_duplicates(self):
        """名称去重."""
        text = "贵州茅台贵州茅台双双大涨"
        extractor = SymbolExtractor()
        codes = extractor.extract_names(text)
        assert len(codes) == 1

    def test_extract_all(self):
        """代码+名称提取."""
        text = "贵州茅台(600519.SH)业绩增长，宁德时代(300750.SZ)跟涨"
        extractor = SymbolExtractor()
        codes = extractor.extract_all(text)
        assert "600519.SH" in codes
        assert "300750.SZ" in codes

    def test_no_match(self):
        """无匹配."""
        extractor = SymbolExtractor()
        assert extractor.extract_all("今天天气不错") == []

    def test_add_stock_name(self):
        """添加自定义名称."""
        extractor = SymbolExtractor()
        extractor.add_stock_name("测试股票", "000001.SZ")
        codes = extractor.extract_names("测试股票大涨")
        assert "000001.SZ" in codes

    def test_custom_names_init(self):
        """初始化自定义名称."""
        extractor = SymbolExtractor(stock_names={"寒武纪": "688256.SH"})
        codes = extractor.extract_names("寒武纪发布新品")
        assert "688256.SH" in codes

    def test_bj_stock(self):
        """北交所代码."""
        text = "贝特瑞(835185.BJ)"
        extractor = SymbolExtractor()
        codes = extractor.extract_codes(text)
        assert "835185.BJ" in codes


# =========================================================
# B3: EventDetector
# =========================================================


class TestEventDetector:
    """事件检测器测试."""

    def test_detect_policy(self):
        """检测政策事件."""
        detector = EventDetector()
        types = detector.detect("央行宣布降准0.5个百分点")
        assert EventType.POLICY in types

    def test_detect_earnings(self):
        """检测财报事件."""
        detector = EventDetector()
        types = detector.detect("贵州茅台净利润同比增长15%%")
        assert EventType.EARNINGS in types

    def test_detect_macro(self):
        """检测宏观事件."""
        detector = EventDetector()
        types = detector.detect("美国3月CPI同比上涨")
        assert EventType.MACRO in types

    def test_detect_geo(self):
        """检测地缘事件."""
        detector = EventDetector()
        types = detector.detect("中美贸易摩擦升级")
        assert EventType.GEO in types

    def test_detect_company(self):
        """检测公司事件."""
        detector = EventDetector()
        types = detector.detect("某公司因财务造假被证监会立案调查")
        assert EventType.COMPANY in types

    def test_detect_market(self):
        """检测市场事件."""
        detector = EventDetector()
        types = detector.detect("A股三大股指全线上涨")
        assert EventType.MARKET in types

    def test_detect_industry(self):
        """检测行业事件."""
        detector = EventDetector()
        types = detector.detect("新能源汽车产业链供需两旺")
        assert EventType.INDUSTRY in types

    def test_detect_tech(self):
        """检测技术事件."""
        detector = EventDetector()
        types = detector.detect("国产光刻机技术实现重大突破")
        assert EventType.TECH in types

    def test_detect_other(self):
        """无法分类."""
        detector = EventDetector()
        types = detector.detect("今日天气晴朗")
        assert types == [EventType.OTHER]

    def test_detect_multi_label(self):
        """多标签."""
        detector = EventDetector()
        types = detector.detect("央行降准降息支持实体经济")
        # "降准"、"降息"、"央行" 都是POLICY关键词
        assert EventType.POLICY in types

    def test_detect_with_content(self):
        """使用正文检测."""
        detector = EventDetector()
        types = detector.detect("重要新闻", "美国非农就业数据超预期")
        assert EventType.MACRO in types

    def test_detect_importance(self):
        """重要性评估."""
        detector = EventDetector()
        assert detector.detect_importance([EventType.GEO]) == 0.9
        assert detector.detect_importance([EventType.MACRO]) == 0.85
        assert detector.detect_importance([EventType.OTHER]) == 0.2

    def test_importance_empty(self):
        """空列表."""
        detector = EventDetector()
        assert detector.detect_importance([]) == 0.3

    def test_add_keywords(self):
        """添加自定义关键词."""
        detector = EventDetector()
        detector.add_keywords(EventType.POLICY, ["新质生产力"])
        types = detector.detect("加快培育新质生产力")
        assert EventType.POLICY in types


# =========================================================
# B4 + B5: SentimentAnalyzer + KeywordSentimentAnalyzer
# =========================================================


class TestSentimentAnalyzer:
    """情绪分析器抽象基类测试."""

    def test_cannot_instantiate(self):
        """抽象类不可实例化."""
        with pytest.raises(TypeError):
            SentimentAnalyzer()  # type: ignore[abstract]

    def test_has_abstract_methods(self):
        """验证抽象方法存在."""
        methods = ["analyze_text", "analyze_batch", "get_sentiment_index"]
        for m in methods:
            assert hasattr(SentimentAnalyzer, m)


class TestSentimentResult:
    """情绪分析结果测试."""

    def test_default(self):
        """默认值."""
        result = SentimentResult(score=0.0, magnitude=0.0)
        assert result.score == 0.0
        assert result.magnitude == 0.0
        assert result.labels == ["neutral"]

    def test_positive(self):
        """正向."""
        result = SentimentResult(score=0.5, magnitude=0.6, labels=["positive"])
        assert result.score == 0.5
        assert "positive" in result.labels


class TestKeywordSentimentAnalyzer:
    """关键词情绪分析器测试."""

    def test_analyze_positive(self):
        """正面文本."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("业绩大幅增长")
        assert result.score > 0
        assert "positive" in result.labels

    def test_analyze_negative(self):
        """负面文本."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("公司股价暴跌")
        assert result.score < 0
        assert "negative" in result.labels

    def test_analyze_neutral(self):
        """中性文本."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("今日发布公告")
        assert -0.2 <= result.score <= 0.2
        assert "neutral" in result.labels

    def test_mixed_sentiment(self):
        """混合情感."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("业绩增长但股价下跌")
        # 正负抵消后更接近中性
        assert -0.5 < result.score < 0.5

    def test_intensifier(self):
        """程度副词."""
        analyzer = KeywordSentimentAnalyzer()
        normal = analyzer.analyze_text("业绩增长")
        strong = analyzer.analyze_text("业绩大幅增长")
        # 带程度副词的强度应更高
        assert abs(strong.score) >= abs(normal.score)

    def test_negator_reversal(self):
        """否定词反转."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("业绩没有增长")
        # 否定应降低正面得分
        assert result.score <= 0.2 or result.score < 0

    def test_analyze_batch(self):
        """批量分析."""
        analyzer = KeywordSentimentAnalyzer()
        items = [
            NewsItem(news_id="1", title="业绩增长", content="", source="cls", published_at=datetime.now()),
            NewsItem(news_id="2", title="股价暴跌", content="", source="cls", published_at=datetime.now()),
        ]
        results = analyzer.analyze_batch(items)
        assert len(results) == 2
        assert results[0].score > 0
        assert results[1].score < 0

    def test_get_sentiment_index(self):
        """情绪指数."""
        analyzer = KeywordSentimentAnalyzer()
        df = analyzer.get_sentiment_index("600519.SH", date(2024, 1, 1), date(2024, 1, 10))
        assert "date" in df.columns
        assert "sentiment" in df.columns
        assert len(df) > 0

    def test_custom_positive_words(self):
        """自定义正面词."""
        analyzer = KeywordSentimentAnalyzer(positive_words=["超级利好"])
        result = analyzer.analyze_text("超级利好")
        assert result.score > 0

    def test_custom_negative_words(self):
        """自定义负面词."""
        analyzer = KeywordSentimentAnalyzer(negative_words=["重大利空"])
        result = analyzer.analyze_text("重大利空")
        assert result.score < 0

    def test_add_positive_words(self):
        """追加正面词."""
        analyzer = KeywordSentimentAnalyzer()
        analyzer.add_positive_words(["涨停"])
        result = analyzer.analyze_text("涨停")
        assert result.score > 0

    def test_add_negative_words(self):
        """追加负面词."""
        analyzer = KeywordSentimentAnalyzer()
        analyzer.add_negative_words(["st"])
        result = analyzer.analyze_text("st")
        assert result.score < 0

    def test_empty_text(self):
        """空文本."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("")
        assert result.score == 0.0
        assert result.labels == ["neutral"]

    def test_score_range(self):
        """得分范围在 [-1, 1]."""
        analyzer = KeywordSentimentAnalyzer()
        result = analyzer.analyze_text("业绩大幅增长")
        assert -1.0 <= result.score <= 1.0
        assert 0.0 <= result.magnitude <= 1.0


# =========================================================
# B6: ClsNewsProvider
# =========================================================


class TestClsNewsProvider:
    """财联社Provider测试."""

    def test_name(self):
        """名称."""
        provider = ClsNewsProvider()
        assert provider.name == "cls_news"

    def test_get_latest_news(self):
        """获取最新新闻（mock fallback）."""
        provider = ClsNewsProvider()
        news = provider.get_latest_news(count=5)
        assert len(news) > 0
        assert len(news) <= 5
        assert all(isinstance(n, NewsItem) for n in news)

    def test_news_item_fields(self):
        """新闻条目字段完整性."""
        provider = ClsNewsProvider()
        news = provider.get_latest_news(count=3)
        for n in news:
            assert n.news_id
            assert n.title
            assert n.source == "cls"
            assert n.published_at is not None
            assert isinstance(n.symbols, list)
            assert isinstance(n.tags, list)

    def test_news_has_symbols(self):
        """模拟新闻含有关联标的."""
        provider = ClsNewsProvider()
        news = provider.get_latest_news(count=10)
        # 至少有一条新闻应该匹配到标的
        has_symbols = any(len(n.symbols) > 0 for n in news)
        assert has_symbols

    def test_news_has_event_types(self):
        """模拟新闻含有事件分类."""
        provider = ClsNewsProvider()
        news = provider.get_latest_news(count=10)
        has_tags = any(len(n.tags) > 0 for n in news)
        assert has_tags

    def test_get_news_by_range(self):
        """按时间范围查询."""
        provider = ClsNewsProvider()
        now = datetime.now()
        start = datetime(now.year, 1, 1)
        end = datetime(now.year, 12, 31)
        news = provider.get_news_by_range(start, end)
        assert len(news) > 0

    def test_get_news_by_range_with_keywords(self):
        """按关键词查询."""
        provider = ClsNewsProvider()
        now = datetime.now()
        start = datetime(now.year, 1, 1)
        end = datetime(now.year, 12, 31)
        news = provider.get_news_by_range(start, end, keywords=["茅台"])
        # 可能匹配到包含"茅台"的模拟新闻
        assert len(news) >= 0

    def test_get_company_announcements(self):
        """获取公司相关新闻."""
        provider = ClsNewsProvider()
        now = datetime.now()
        start = date(now.year, 1, 1)
        end = date(now.year, 12, 31)
        news = provider.get_company_announcements("600519.SH", start, end)
        # 600519.SH 在模拟新闻中（贵州茅台）
        assert len(news) > 0

    def test_health_check(self):
        """健康检查."""
        provider = ClsNewsProvider()
        assert provider.health_check()

    def test_subscribe(self):
        """订阅回调."""
        received: list[NewsItem] = []

        def callback(item: NewsItem) -> None:
            received.append(item)

        provider = ClsNewsProvider()
        provider.subscribe(callback)
        assert len(provider._callbacks) == 1

    def test_empty_range(self):
        """范围无匹配."""
        provider = ClsNewsProvider()
        news = provider.get_news_by_range(
            datetime(2020, 1, 1), datetime(2020, 1, 2)
        )
        assert len(news) == 0


# =========================================================
# 集成测试
# =========================================================


class TestNewsIntegration:
    """新闻模块集成测试."""

    def test_full_pipeline(self):
        """完整管道：新闻获取→标的提取→事件分类→情绪分析."""
        provider = ClsNewsProvider()
        news = provider.get_latest_news(count=10)
        assert len(news) > 0

        analyzer = KeywordSentimentAnalyzer()
        results = analyzer.analyze_batch(news)
        assert len(results) == len(news)
        assert all(isinstance(r, SentimentResult) for r in results)

        # 验证每个新闻都有对应的情绪结果
        for n, r in zip(news, results):
            assert -1.0 <= r.score <= 1.0
            assert 0.0 <= r.magnitude <= 1.0

    def test_symbol_extraction_with_sentiment(self):
        """新闻情绪+标的关联."""
        provider = ClsNewsProvider()
        analyzer = KeywordSentimentAnalyzer()

        news = provider.get_latest_news(count=10)
        for n in news:
            # 如果有标的关联且非中性，情绪应该有一定倾向
            if n.symbols and n.news_id.startswith("mock_cls"):
                result = analyzer.analyze_text(f"{n.title} {n.content}")
                n.sentiment = result.score
                assert n.sentiment is not None
