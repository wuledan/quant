#!/usr/bin/env python3
"""宏观数据发布日程."""

from __future__ import annotations

from datetime import date, timedelta

import pandas as pd


class MacroCalendar:
    """宏观数据发布日程管理器."""

    def __init__(self) -> None:
        self._events: list[dict] = []

    def add_event(self, name: str, release_date: date, country: str = "CN", source: str = "") -> None:
        """添加发布事件."""
        self._events.append({
            "name": name, "release_date": release_date,
            "country": country, "source": source,
        })

    def get_upcoming(self, days: int = 7, country: str = "") -> list[dict]:
        """获取未来N天内的发布日程."""
        today = pd.Timestamp.now().date()
        end = today + timedelta(days=days)
        results = []
        for evt in self._events:
            if today <= evt["release_date"] <= end:
                if not country or evt["country"] == country:
                    results.append(evt)
        return sorted(results, key=lambda x: x["release_date"])

    def get_by_month(self, year: int, month: int) -> list[dict]:
        """获取指定月份的发布日程."""
        return [
            evt for evt in self._events
            if evt["release_date"].year == year and evt["release_date"].month == month
        ]

    @property
    def all_events(self) -> list[dict]:
        return list(self._events)

    @staticmethod
    def get_china_schedule() -> "MacroCalendar":
        """返回中国宏观数据发布日程模板."""
        cal = MacroCalendar()
        now = date.today()
        year = now.year
        cal.add_event("GDP", date(year, 1, 17), "CN", "国家统计局")
        cal.add_event("GDP", date(year, 4, 17), "CN", "国家统计局")
        cal.add_event("CPI/PPI", date(year, 1, 9), "CN", "国家统计局")
        cal.add_event("PMI", date(year, 1, 1), "CN", "国家统计局")
        cal.add_event("M2/社融", date(year, 1, 10), "CN", "央行")
        cal.add_event("进出口", date(year, 1, 14), "CN", "海关")
        cal.add_event("工业增加值", date(year, 1, 17), "CN", "国家统计局")
        cal.add_event("社零", date(year, 1, 17), "CN", "国家统计局")
        cal.add_event("固定资产投资", date(year, 1, 17), "CN", "国家统计局")
        cal.add_event("LPR", date(year, 1, 20), "CN", "央行")
        return cal
