#!/usr/bin/env python3
"""新闻数据路由 — 优先从调度器缓存提供."""

from __future__ import annotations

from datetime import date, datetime

from fastapi import APIRouter, Query

from ...data.scheduler import get_scheduler

router = APIRouter()


@router.get("/latest")
async def get_latest_news(
    count: int = Query(50, description="获取条数"),
) -> dict:
    """获取最新新闻 — 优先本地缓存."""
    try:
        scheduler = get_scheduler()
        news = scheduler.get_news(count=count)
        if news:
            return {"news": news, "total": len(news)}

        # 缓存未命中时实时拉取
        from quant_invest.data.providers.cls_news_provider import ClsNewsProvider
        provider = ClsNewsProvider()
        items = provider.get_latest_news(count=count)
        return {
            "news": [
                {
                    "id": item.news_id,
                    "title": item.title,
                    "content": item.content[:200],
                    "source": item.source,
                    "published_at": item.published_at.isoformat(),
                    "symbols": item.symbols,
                    "tags": item.tags,
                    "importance": item.importance,
                }
                for item in items
            ],
            "total": len(items),
        }
    except Exception as e:
        return {"news": [], "total": 0, "error": str(e)}


@router.get("/range")
async def get_news_by_range(
    start: str = Query(..., description="开始时间 ISO格式"),
    end: str = Query(..., description="结束时间 ISO格式"),
    keywords: str | None = Query(None, description="关键词逗号分隔"),
) -> dict:
    """按时间范围查询新闻."""
    try:
        start_dt = datetime.fromisoformat(start)
        end_dt = datetime.fromisoformat(end)
        kw_list = keywords.split(",") if keywords else None

        from quant_invest.data.providers.cls_news_provider import ClsNewsProvider
        provider = ClsNewsProvider()
        items = provider.get_news_by_range(start_dt, end_dt, keywords=kw_list)
        return {
            "news": [
                {
                    "id": item.news_id,
                    "title": item.title,
                    "content": item.content[:200],
                    "source": item.source,
                    "published_at": item.published_at.isoformat(),
                    "symbols": item.symbols,
                    "tags": item.tags,
                    "importance": item.importance,
                }
                for item in items
            ],
            "total": len(items),
        }
    except Exception as e:
        return {"news": [], "total": 0, "error": str(e)}


@router.get("/company/{symbol}")
async def get_company_news(
    symbol: str,
    start_date: str = Query("", description="开始日期"),
    end_date: str = Query("", description="结束日期"),
) -> dict:
    """获取公司相关新闻."""
    try:
        sd = date.fromisoformat(start_date) if start_date else date(2024, 1, 1)
        ed = date.fromisoformat(end_date) if end_date else date.today()

        from quant_invest.data.providers.cls_news_provider import ClsNewsProvider
        provider = ClsNewsProvider()
        items = provider.get_company_announcements(symbol, sd, ed)
        return {
            "news": [
                {
                    "id": item.news_id,
                    "title": item.title,
                    "content": item.content[:200],
                    "source": item.source,
                    "published_at": item.published_at.isoformat(),
                    "symbols": item.symbols,
                    "tags": item.tags,
                    "importance": item.importance,
                }
                for item in items
            ],
            "total": len(items),
        }
    except Exception as e:
        return {"news": [], "total": 0, "error": str(e)}


@router.get("/health")
async def news_health() -> dict:
    """新闻源健康检查."""
    scheduler = get_scheduler()
    status = scheduler.get_status()
    has_news = status.get("news_cache_count", 0) > 0
    return {"status": "ok" if has_news else "no_data", "provider": "cls_news", "cache_count": status.get("news_cache_count", 0)}
