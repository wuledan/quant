#!/usr/bin/env python3
"""宏观与资金面数据测试."""

from __future__ import annotations

from datetime import date

import pandas as pd
import pytest

from quant_invest.data.macro import CapitalFlowProvider, MacroCalendar, MacroIndicators, MacroProvider
from quant_invest.data.providers.akshare_macro_provider import AkshareMacroProvider


class TestMacroProvider:
    """宏观数据接口测试."""

    def test_provider_abstract(self):
        """接口不能直接实例化."""
        with pytest.raises(TypeError):
            MacroProvider()  # type: ignore[abstract]

    def test_akshare_macro_provider_name(self):
        """Provider名称."""
        p = AkshareMacroProvider()
        assert p.name == "akshare_macro"

    def test_akshare_macro_is_instance(self):
        """同时实现MacroProvider和CapitalFlowProvider."""
        p = AkshareMacroProvider()
        assert isinstance(p, MacroProvider)
        assert isinstance(p, CapitalFlowProvider)

    def test_get_china_gdp(self):
        """中国GDP."""
        p = AkshareMacroProvider()
        df = p.get_china_gdp(date(2024, 1, 1), date(2024, 12, 31))
        assert not df.empty
        assert "value" in df.columns

    def test_get_china_cpi(self):
        """中国CPI."""
        p = AkshareMacroProvider()
        df = p.get_china_cpi(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_china_pmi(self):
        """中国PMI."""
        p = AkshareMacroProvider()
        df = p.get_china_pmi(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_china_money_supply(self):
        """M2."""
        p = AkshareMacroProvider()
        df = p.get_china_money_supply(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_china_social_financing(self):
        """社融."""
        p = AkshareMacroProvider()
        df = p.get_china_social_financing(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_china_interest_rate(self):
        """利率."""
        p = AkshareMacroProvider()
        df = p.get_china_interest_rate(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_fx_rate(self):
        """汇率."""
        p = AkshareMacroProvider()
        df = p.get_fx_rate("USD/CNY", date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_us_fed_rate(self):
        """美联储利率."""
        p = AkshareMacroProvider()
        df = p.get_us_fed_rate(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_us_cpi(self):
        """美国CPI."""
        p = AkshareMacroProvider()
        df = p.get_us_cpi(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_us_nonfarm(self):
        """非农."""
        p = AkshareMacroProvider()
        df = p.get_us_nonfarm(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_intl_commodity(self):
        """国际商品."""
        p = AkshareMacroProvider()
        df = p.get_intl_commodity("黄金", date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_health_check(self):
        """健康检查."""
        p = AkshareMacroProvider()
        assert p.health_check()

    def test_with_config(self):
        """带配置初始化."""
        p = AkshareMacroProvider(config={"timeout": 30})
        assert p._config["timeout"] == 30


class TestCapitalFlowProvider:
    """资金面数据测试."""

    def test_get_northbound_flow(self):
        """北向资金."""
        p = AkshareMacroProvider()
        df = p.get_northbound_flow(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_margin_trading(self):
        """融资融券."""
        p = AkshareMacroProvider()
        df = p.get_margin_trading("000001.SZ", date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_block_trade(self):
        """大宗交易."""
        p = AkshareMacroProvider()
        df = p.get_block_trade(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_etf_flow(self):
        """ETF资金流."""
        p = AkshareMacroProvider()
        df = p.get_etf_flow("510050", date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty

    def test_get_pbc_operation(self):
        """央行操作."""
        p = AkshareMacroProvider()
        df = p.get_pbc_operation(date(2024, 1, 1), date(2024, 6, 30))
        assert not df.empty


class TestMacroIndicators:
    """宏观指标计算测试."""

    def test_calc_mom(self):
        """环比."""
        series = pd.Series([100, 102, 105, 103], index=pd.date_range("2024-01", periods=4, freq="ME"))
        mom = MacroIndicators.calc_mom(series)
        assert pd.isna(mom.iloc[0])  # first is NaN
        assert abs(mom.iloc[1] - (102/100 - 1)) < 1e-6

    def test_calc_yoy(self):
        """同比."""
        series = pd.Series(range(6, 30), index=pd.date_range("2022-01", periods=24, freq="ME"))
        yoy = MacroIndicators.calc_yoy(series, periods=12)
        # iloc[0]=6, iloc[12]=18, yoy = (18-6)/6 = 2.0
        assert yoy.iloc[12] == pytest.approx(2.0)

    def test_leading_indicator(self):
        """领先指标合成."""
        dates = pd.date_range("2024-01", periods=10, freq="ME")
        indicators = {
            "pmi": pd.Series(range(10), index=dates),
            "m2": pd.Series(range(10, 20), index=dates),
        }
        result = MacroIndicators.calc_leading_indicator(indicators)
        assert len(result) == 10
        assert abs(result.mean()) < 1e-10  # 标准化后均值为0

    def test_leading_indicator_empty(self):
        """空输入."""
        result = MacroIndicators.calc_leading_indicator({})
        assert len(result) == 0

    def test_composite_index(self):
        """定基指数."""
        series = pd.Series([100, 110, 120], index=pd.date_range("2024-01", periods=3, freq="ME"))
        idx = MacroIndicators.calc_composite_index(series, base_value=100)
        assert idx.iloc[0] == 100.0
        assert idx.iloc[1] == pytest.approx(110.0)

    def test_moving_average(self):
        """移动平均."""
        series = pd.Series([1, 2, 3, 4, 5])
        ma = MacroIndicators.calc_moving_average(series, window=3)
        assert pd.isna(ma.iloc[0])
        assert ma.iloc[2] == 2.0

    def test_diff_from_trend(self):
        """偏离趋势."""
        series = pd.Series([100, 101, 102, 103, 104])
        diff = MacroIndicators.calc_diff_from_trend(series, trend_window=3)
        # trend = [NaN, NaN, 101, 102, 103]
        # diff[4] = (104 / 103 - 1) * 100
        assert not pd.isna(diff.iloc[-1])

    def test_classify_regime(self):
        """经济周期分类."""
        gdp = pd.Series([3.0, 5.0, 5.0, 2.0], index=pd.date_range("2024-01", periods=4, freq="QE"))
        cpi = pd.Series([1.0, 1.0, 3.0, 3.0], index=pd.date_range("2024-01", periods=4, freq="QE"))
        pmi = pd.Series([50, 52, 51, 48], index=pd.date_range("2024-01", periods=4, freq="QE"))
        regimes = MacroIndicators.classify_regime(gdp, cpi, pmi)
        assert len(regimes) == 4


class TestMacroCalendar:
    """宏观发布日程测试."""

    def test_add_event(self):
        """添加事件."""
        cal = MacroCalendar()
        cal.add_event("CPI", date(2024, 1, 15))
        assert len(cal.all_events) == 1

    def test_get_by_month(self):
        """按月查询."""
        cal = MacroCalendar()
        cal.add_event("CPI", date(2024, 1, 15))
        cal.add_event("GDP", date(2024, 4, 17))
        jan_events = cal.get_by_month(2024, 1)
        assert len(jan_events) == 1
        assert jan_events[0]["name"] == "CPI"

    def test_china_schedule(self):
        """中国发布日程."""
        cal = MacroCalendar.get_china_schedule()
        names = {e["name"] for e in cal.all_events}
        assert "GDP" in names
        assert "CPI/PPI" in names

    def test_get_upcoming_empty(self):
        """无即将发布事件."""
        cal = MacroCalendar()
        cal.add_event("CPI", date(2020, 1, 15))
        upcoming = cal.get_upcoming(days=7)
        assert len(upcoming) == 0
