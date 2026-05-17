#!/usr/bin/env python3
"""数据查询路由.

端点:
- GET  /daily/{symbol}        日线数据
- GET  /minute/{symbol}       分钟线数据
- GET  /financial/{symbol}    财务数据
- GET  /macro/{indicator}     宏观数据
- GET  /trade_calendar        交易日历
- GET  /index_constituents    指数成分股
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query, status

from ..schemas import DailyBarResponse, FinancialDataResponse, MacroDataResponse, MinuteBarResponse

router = APIRouter()


@router.get("/daily/{symbol}", response_model=DailyBarResponse)
async def get_daily_bars(
    symbol: str,
    start_date: str = Query("", description="开始日期 YYYY-MM-DD"),
    end_date: str = Query("", description="结束日期 YYYY-MM-DD"),
    adjust: str = Query("none", description="复权方式 none/forward/backward"),
    limit: int = Query(1000, description="返回条数", le=10000),
) -> DailyBarResponse:
    """获取日线数据."""
    return DailyBarResponse(symbol=symbol, data=[], count=0)


@router.get("/minute/{symbol}", response_model=MinuteBarResponse)
async def get_minute_bars(
    symbol: str,
    date: str = Query("", description="交易日期 YYYY-MM-DD"),
    frequency: str = Query("1min", description="频率 1min/5min/15min"),
    limit: int = Query(1000, description="返回条数", le=10000),
) -> MinuteBarResponse:
    """获取分钟线数据."""
    return MinuteBarResponse(symbol=symbol, date=date, data=[], count=0)


@router.get("/financial/{symbol}", response_model=FinancialDataResponse)
async def get_financial(
    symbol: str,
    report_type: str = Query("income", description="报表类型 income/balance/cashflow"),
) -> FinancialDataResponse:
    """获取财务数据."""
    return FinancialDataResponse(symbol=symbol, report_type=report_type)


@router.get("/macro/{indicator}", response_model=MacroDataResponse)
async def get_macro_data(
    indicator: str,
    start_date: str = Query("", description="开始日期 YYYY-MM-DD"),
    end_date: str = Query("", description="结束日期 YYYY-MM-DD"),
) -> MacroDataResponse:
    """获取宏观数据.

    indicator 可选: gdp, cpi, pmi, m2, shibor, northbound
    """
    valid = {"gdp", "cpi", "pmi", "m2", "shibor", "northbound"}
    if indicator not in valid:
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=f"Unknown indicator '{indicator}'. Choose from {sorted(valid)}",
        )
    return MacroDataResponse(indicator=indicator, data=[], count=0)


@router.get("/trade_calendar")
async def get_trade_calendar(
    start_date: str = Query(..., description="开始日期 YYYY-MM-DD"),
    end_date: str = Query(..., description="结束日期 YYYY-MM-DD"),
) -> dict:
    """获取交易日历."""
    return {"dates": []}


@router.get("/index_constituents")
async def get_index_constituents(
    index_code: str = Query(..., description="指数代码"),
) -> dict:
    """获取指数成分股."""
    return {"index_code": index_code, "constituents": []}
