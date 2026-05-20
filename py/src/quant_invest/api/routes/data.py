#!/usr/bin/env python3
"""数据查询路由 — 优先从本地缓存提供, 缓存未命中时实时拉取."""

from __future__ import annotations

from datetime import date

from fastapi import APIRouter, HTTPException, Query

from ..schemas import DailyBarResponse
from ...data.scheduler import get_scheduler

router = APIRouter()


@router.get("/daily/{symbol}", response_model=DailyBarResponse)
async def get_daily_bars(
    symbol: str,
    start_date: str = Query("", description="开始日期 YYYY-MM-DD"),
    end_date: str = Query("", description="结束日期 YYYY-MM-DD"),
    adjust: str = Query("forward", description="复权方式 none/forward/backward"),
) -> DailyBarResponse:
    """获取日线数据 — 优先本地缓存, 缓存无数据时实时拉取."""
    try:
        sd = date.fromisoformat(start_date) if start_date else None
        ed = date.fromisoformat(end_date) if end_date else None

        scheduler = get_scheduler()
        df = scheduler.get_daily_data(symbol, start_date=sd, end_date=ed)

        if df is None or df.empty:
            # 缓存无数据，尝试实时拉取
            from quant_invest.data.providers import DataProviderFactory
            provider = DataProviderFactory.create("yahoo")
            fetch_sd = sd or date(date.today().year - 1, 1, 1)
            fetch_ed = ed or date.today()
            df = provider.get_daily_bars(symbol, fetch_sd, fetch_ed)

        if df is None or df.empty:
            return DailyBarResponse(symbol=symbol, data=[], count=0)

        records = []
        for idx, row in df.iterrows():
            records.append({
                "date": idx.strftime("%Y-%m-%d") if hasattr(idx, "strftime") else str(idx),
                "open": float(row["open"]),
                "high": float(row["high"]),
                "low": float(row["low"]),
                "close": float(row["close"]),
                "volume": float(row["volume"]),
                "amount": float(row.get("amount", 0)),
            })
        return DailyBarResponse(symbol=symbol, data=records, count=len(records))
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/trade_calendar")
async def get_trade_calendar(
    start_date: str = Query(..., description="开始日期 YYYY-MM-DD"),
    end_date: str = Query(..., description="结束日期 YYYY-MM-DD"),
) -> dict:
    """获取交易日历."""
    try:
        sd = date.fromisoformat(start_date)
        ed = date.fromisoformat(end_date)
        scheduler = get_scheduler()
        df = scheduler.get_daily_data("000300.SH", start_date=sd, end_date=ed)
        if df is not None and not df.empty:
            dates = [d.strftime("%Y-%m-%d") for d in df.index]
            return {"dates": dates}
        # fallback: 实时拉取
        from quant_invest.data.providers import DataProviderFactory
        provider = DataProviderFactory.create("yahoo")
        trade_dates = provider.get_trade_calendar(sd, ed)
        return {"dates": [d.strftime("%Y-%m-%d") for d in trade_dates]}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/index_constituents")
async def get_index_constituents(
    index_code: str = Query(..., description="指数代码 如 000300.SH"),
) -> dict:
    """获取指数成分股."""
    try:
        from quant_invest.data.providers import DataProviderFactory
        provider = DataProviderFactory.create("yahoo")
        df = provider.get_index_constituents(index_code)
        return {"index_code": index_code, "constituents": df.to_dict(orient="records") if not df.empty else []}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/health")
async def data_health() -> dict:
    """数据源健康检查."""
    scheduler = get_scheduler()
    status = scheduler.get_status()
    return {"status": "ok" if status["running"] else "stopped", "cache": status}


@router.get("/scheduler_status")
async def scheduler_status() -> dict:
    """调度器详细状态."""
    scheduler = get_scheduler()
    return scheduler.get_status()
