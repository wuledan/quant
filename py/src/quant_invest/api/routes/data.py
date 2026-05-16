#!/usr/bin/env python3
"""数据查询路由."""

from __future__ import annotations

from fastapi import APIRouter, Query

router = APIRouter()


@router.get("/kline")
async def get_kline(
    symbol: str = Query(..., description="标的代码"),
    frequency: str = Query("daily", description="数据频率 daily/weekly/monthly"),
    start_date: str = Query("", description="开始日期 YYYY-MM-DD"),
    end_date: str = Query("", description="结束日期 YYYY-MM-DD"),
    adjust: str = Query("none", description="复权方式 none/forward/backward"),
    limit: int = Query(1000, description="返回条数", le=10000),
):
    """获取K线数据."""
    return {
        "symbol": symbol,
        "frequency": frequency,
        "data": [],
    }


@router.get("/tick")
async def get_tick(
    symbol: str = Query(..., description="标的代码"),
    date: str = Query(..., description="交易日期 YYYY-MM-DD"),
    limit: int = Query(1000, description="返回条数", le=10000),
):
    """获取Tick数据."""
    return {
        "symbol": symbol,
        "date": date,
        "data": [],
    }


@router.get("/financial")
async def get_financial(
    symbol: str = Query(..., description="标的代码"),
    report_type: str = Query("income", description="报表类型 income/balance/cashflow"),
    year: int = Query(2024, description="年度"),
    quarter: int = Query(1, description="季度", ge=1, le=4),
):
    """获取财务数据."""
    return {
        "symbol": symbol,
        "report_type": report_type,
        "data": {},
    }


@router.get("/trade_calendar")
async def get_trade_calendar(
    start_date: str = Query(..., description="开始日期 YYYY-MM-DD"),
    end_date: str = Query(..., description="结束日期 YYYY-MM-DD"),
):
    """获取交易日历."""
    return {"dates": []}


@router.get("/index_constituents")
async def get_index_constituents(
    index_code: str = Query(..., description="指数代码"),
):
    """获取指数成分股."""
    return {"index_code": index_code, "constituents": []}
</｜DSML｜parameter>