# 扩展数据源架构：宏观/新闻/舆情/另类数据

> 版本：v1.0 | 日期：2026-05-16 | 依托现有 DataProvider 体系扩展

---

## 1. 设计目标

当前系统仅覆盖行情+财务数据，缺少宏观环境、新闻事件、市场情绪、另类数据。
这些数据是事件驱动策略和情绪因子的核心输入，补充后可支撑：

- **宏观策略**：基于CPI/PMI/利率/汇率的资产配置与择时
- **事件驱动策略**：政策发布、黑天鹅、地缘冲突的快速反应
- **情绪因子**：新闻/舆情→情绪得分→Alpha因子
- **另类Alpha**：供应链/招聘/专利等非传统数据源的超额收益

---

## 2. 数据源总览

| 数据类别 | 具体内容 | 主要来源 | 更新频率 | 优先级 |
|---------|---------|---------|---------|--------|
| **宏观经济** | GDP/CPI/PPI/PMI/M2/社融/利率/汇率 | akshare(国内) + FRED(海外) | 月/季 | P0 |
| **资金面** | 北向资金/融资融券/大宗交易/ETF申赎/央行公开市场操作 | akshare | 日 | P0 |
| **新闻快讯** | 财联社电报/新浪财经/东方财经 | 财联社API/爬虫 | 实时 | P1 |
| **公告/监管** | 上市公司公告/证监会文件/交易所规则 | 巨潮资讯/akshare | 日 | P1 |
| **社交媒体** | 雪球/股吧/微博财经讨论 | 爬虫 + NLP | 实时 | P2 |
| **研报** | 券商研报摘要/评级调整 | akshare | 日 | P2 |
| **地缘/国际** | 中美关系/贸易政策/美联储/地缘冲突 | FRED + 新闻源 | 事件驱动 | P2 |
| **另类数据** | 供应链关系/高管增减持/招聘/专利 | akshare(基础) + 第三方 | 周/月 | P3 |

---

## 3. 接口设计

### 3.1 宏观数据接口

```python
class MacroProvider(ABC):
    """宏观经济数据源"""

    @property
    @abstractmethod
    def name(self) -> str: ...

    # === 中国宏观 ===
    @abstractmethod
    def get_china_gdp(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_cpi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_pmi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_china_money_supply(self, start: date, end: date) -> pd.DataFrame:
        """M0/M1/M2""" ...

    @abstractmethod
    def get_china_social_financing(self, start: date, end: date) -> pd.DataFrame:
        """社会融资规模""" ...

    @abstractmethod
    def get_china_interest_rate(self, start: date, end: date) -> pd.DataFrame:
        """Shibor/LPR/国债收益率""" ...

    @abstractmethod
    def get_fx_rate(self, pair: str, start: date, end: date) -> pd.DataFrame:
        """汇率: USD/CNY, EUR/CNY 等""" ...

    # === 海外宏观 ===
    @abstractmethod
    def get_us_fed_rate(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_us_cpi(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_us_nonfarm(self, start: date, end: date) -> pd.DataFrame: ...

    @abstractmethod
    def get_intl_commodity(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """国际商品: 原油/黄金/铜/大豆""" ...

    @abstractmethod
    def health_check(self) -> bool: ...
```

### 3.2 资金面数据接口

```python
class CapitalFlowProvider(ABC):
    """资金面数据源"""

    @property
    @abstractmethod
    def name(self) -> str: ...

    @abstractmethod
    def get_northbound_flow(self, start: date, end: date) -> pd.DataFrame:
        """北向资金日流向""" ...

    @abstractmethod
    def get_margin_trading(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """融资融券数据""" ...

    @abstractmethod
    def get_block_trade(self, start: date, end: date) -> pd.DataFrame:
        """大宗交易""" ...

    @abstractmethod
    def get_etf_flow(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """ETF申赎资金流""" ...

    @abstractmethod
    def get_pbc_operation(self, start: date, end: date) -> pd.DataFrame:
        """央行公开市场操作""" ...

    @abstractmethod
    def health_check(self) -> bool: ...
```

### 3.3 新闻/事件数据接口

```python
class NewsProvider(ABC):
    """新闻与事件数据源"""

    @property
    @abstractmethod
    def name(self) -> str: ...

    @abstractmethod
    def get_latest_news(self, count: int = 50) -> list[NewsItem]:
        """获取最新新闻列表""" ...

    @abstractmethod
    def get_news_by_range(
        self, start: datetime, end: datetime, keywords: list[str] | None = None
    ) -> list[NewsItem]:
        """按时间范围+关键词查询新闻""" ...

    @abstractmethod
    def get_company_announcements(
        self, symbol: str, start: date, end: date
    ) -> list[NewsItem]:
        """上市公司公告""" ...

    @abstractmethod
    def subscribe(self, callback: Callable[[NewsItem], None]) -> None:
        """订阅实时新闻推送（可选实现）""" ...

    @abstractmethod
    def health_check(self) -> bool: ...


@dataclass
class NewsItem:
    """新闻条目"""
    news_id: str
    title: str
    content: str
    source: str              # cls/sina/eastmoney
    published_at: datetime
    symbols: list[str]       # 关联标的
    tags: list[str]          # 分类标签: policy/earnings/macro/geo
    sentiment: float | None  # 情绪得分 [-1, 1]，由SentimentAnalyzer填充
    importance: float         # 重要性 [0, 1]
```

### 3.4 舆情分析接口

```python
class SentimentAnalyzer(ABC):
    """舆情分析器"""

    @abstractmethod
    def analyze_text(self, text: str) -> SentimentResult:
        """单条文本情绪分析""" ...

    @abstractmethod
    def analyze_batch(self, items: list[NewsItem]) -> list[SentimentResult]:
        """批量情绪分析""" ...

    @abstractmethod
    def get_sentiment_index(
        self, symbol: str, start: date, end: date
    ) -> pd.DataFrame:
        """获取某标的的日级情绪指数""" ...


@dataclass
class SentimentResult:
    """情绪分析结果"""
    score: float        # [-1, 1], 负面→正面
    magnitude: float    # [0, 1], 情绪强度
    labels: list[str]   # 情绪标签: positive/negative/neutral/fear/greed
```

---

## 4. 实现方案

### 4.1 AkshareMacroProvider（P0，免费）

akshare 已有丰富的宏观接口，直接封装：

| 方法 | akshare对应 |
|------|-----------|
| `get_china_gdp` | `ak.macro_china_gdp()` |
| `get_china_cpi` | `ak.macro_china_cpi_yearly()` / `monthly` |
| `get_china_pmi` | `ak.macro_china_pmi()` |
| `get_china_money_supply` | `ak.macro_china_money_supply()` |
| `get_china_social_financing` | `ak.macro_china_shrzgm()` |
| `get_china_interest_rate` | `ak.rate_interbank()` / `ak.macro_china_lpr()` |
| `get_fx_rate` | `ak.currency_boc_sina()` |
| `get_northbound_flow` | `ak.stock_em_hsgt_north_net_flow_in_em()` |
| `get_us_fed_rate` | `ak.macro_usa_interest_rate()` |

### 4.2 FredMacroProvider（P0，海外宏观）

美联储经济数据（FRED）覆盖美国及全球宏观，免费API key：
- GDP/CPI/Nonfarm/美联储利率/Treasury收益率
- 通过 `fredapi` 库或直接HTTP调用

### 4.3 ClsNewsProvider（P1，财联社电报）

财联社是A股最权威的实时财经快讯源：
- **HTTP轮询模式**：定时拉取最新电报（5秒间隔）
- **WebSocket模式**（需付费）：实时推送
- 提供 `subscribe()` 实时回调
- 自动提取关联标的（正则匹配股票代码/名称）

### 4.4 SentimentAnalyzer 实现（P1）

三种方案，按成熟度递进：

| 方案 | 模型 | 延迟 | 成本 | 精度 |
|------|------|------|------|------|
| **Rule-based** | 关键词+金融词典 | <1ms | 免费 | 中 |
| **FinBERT** | 金融领域BERT微调 | ~50ms/条 | GPU | 高 |
| **LLM API** | GPT-4/Claude API | ~1s/条 | 按token | 最高 |

Phase 1 实现Rule-based（关键词词典+正负向词库），Phase 2 引入FinBERT本地模型。

---

## 5. 数据流架构

```
                         数据采集层
  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐
  │ Akshare    │  │ FRED       │  │ 财联社      │  │ 雪球/股吧   │
  │ Macro/Cap  │  │ Oversea    │  │ News/Event  │  │ Social     │
  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘  └─────┬──────┘
        │               │               │               │
        ▼               ▼               ▼               ▼
  ┌─────────────────────────────────────────────────────────────┐
  │                  DataProvider 统一接口层                      │
  │  MacroProvider  CapitalFlowProvider  NewsProvider            │
  └──────────────────────────┬──────────────────────────────────┘
                             │
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
        ┌──────────┐  ┌──────────┐  ┌──────────┐
        │ C++存储   │  │ 舆情分析  │  │ 因子引擎  │
        │ Engine   │  │ Sentiment│  │ Factor   │
        └──────────┘  └──────────┘  └──────────┘
              │              │              │
              └──────────────┼──────────────┘
                             ▼
                    ┌──────────────┐
                    │ 策略/回测/ML  │
                    └──────────────┘
```

---

## 6. 目录结构

```
py/src/quant_invest/data/
├── providers/
│   ├── base.py                  # 已有
│   ├── akshare_provider.py      # 已有（行情+财务）
│   ├── akshare_macro_provider.py   # 新增：akshare宏观+资金
│   ├── fred_macro_provider.py      # 新增：FRED海外宏观
│   ├── cls_news_provider.py        # 新增：财联社新闻
│   └── social_provider.py          # 新增：雪球/股吧
├── macro/                          # 新增：宏观模块
│   ├── __init__.py
│   ├── base.py                     # MacroProvider/CapitalFlowProvider
│   ├── indicators.py               # 宏观指标计算（MOM/YOY/领先指标）
│   └── calendar.py                 # 宏观发布日历
├── news/                           # 新增：新闻模块
│   ├── __init__.py
│   ├── base.py                     # NewsProvider/NewsItem
│   ├── sentiment.py                # SentimentAnalyzer
│   ├── keyword_sentiment.py        # Rule-based情绪分析
│   ├── event_detector.py           # 事件检测与分类
│   └── symbol_extractor.py         # 标的关联提取
├── quality/                        # 已有
├── scheduler.py                    # 已有
└── cache.py                        # 已有
```

---

## 7. 组件级实现计划

### 模块A：宏观与资金面数据（分配给 python-dev-3）

| # | 组件 | 文件 | 说明 |
|---|------|------|------|
| A1 | MacroProvider接口 | `data/macro/base.py` | 抽象基类，定义get_china_cpi/gdp/pmi等接口 |
| A2 | CapitalFlowProvider接口 | `data/macro/base.py` | 北向/融资融券/大宗交易/央行操作 |
| A3 | AkshareMacroProvider | `data/providers/akshare_macro_provider.py` | 同时实现MacroProvider+CapitalFlowProvider |
| A4 | MacroIndicators | `data/macro/indicators.py` | 环比/同比/领先指标/复合指标计算 |
| A5 | MacroCalendar | `data/macro/calendar.py` | 宏观数据发布日程（CPI每月10号、PMI月末等） |
| A6 | 测试 | `tests/macro/test_macro.py` | 接口mock测试+指标计算单元测试 |

### 模块B：新闻与舆情（分配给 python-dev-3）

| # | 组件 | 文件 | 说明 |
|---|------|------|------|
| B1 | NewsItem + NewsProvider | `data/news/base.py` | 数据结构+抽象接口 |
| B2 | SymbolExtractor | `data/news/symbol_extractor.py` | 正则提取新闻关联的A股代码/名称 |
| B3 | EventDetector | `data/news/event_detector.py` | 新闻分类：policy/earnings/macro/geo/industry |
| B4 | KeywordSentimentAnalyzer | `data/news/keyword_sentiment.py` | 金融词典+规则情绪分析 |
| B5 | SentimentAnalyzer接口 | `data/news/sentiment.py` | 抽象基类+SentimentResult |
| B6 | ClsNewsProvider | `data/providers/cls_news_provider.py` | 财联社HTTP轮询（akshare的cls接口） |
| B7 | 测试 | `tests/news/test_news.py` | 新闻提取/分类/情绪的单元测试 |

### 依赖关系

```
A1,A2 → A3 → A4,A5 → A6
B1 → B2,B3 → B4,B5 → B6 → B7
```

A模块和B模块无依赖，可并行开发。
