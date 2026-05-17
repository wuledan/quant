#!/usr/bin/env python3
"""风控管理路由.

端点:
- GET  /status          风控状态概览
- GET  /rules           列出风控规则
- GET  /check           执行风控检查
- POST /rules/{rule_id}/enable   启用规则
- POST /rules/{rule_id}/disable  禁用规则
- GET  /alerts          风控告警列表
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query, status

from ..schemas import RiskCheckResponse, RiskRuleInfo, RiskStatusResponse

router = APIRouter()

_mock_rules: dict[str, RiskRuleInfo] = {
    "1": RiskRuleInfo(rule_id="1", name="MaxDrawdown", description="最大回撤限制", enabled=True, params={"max_drawdown_pct": 0.1}),
    "2": RiskRuleInfo(rule_id="2", name="Concentration", description="持仓集中度限制", enabled=True, params={"max_concentration_pct": 0.3}),
    "3": RiskRuleInfo(rule_id="3", name="Exposure", description="总敞口限制", enabled=True, params={"max_exposure_ratio": 1.5}),
    "4": RiskRuleInfo(rule_id="4", name="Limit", description="绝对限额", enabled=True, params={"max_order_value": 500000, "max_total_value": 5000000}),
    "5": RiskRuleInfo(rule_id="5", name="MaxPositionSize", description="最大持仓数量", enabled=True, params={"max_position_size": 10000}),
}


@router.get("/status", response_model=RiskStatusResponse)
async def get_risk_status() -> RiskStatusResponse:
    """风控状态概览."""
    active = sum(1 for r in _mock_rules.values() if r.enabled)
    return RiskStatusResponse(
        overall_status="normal",
        risk_level="low",
        active_rules=active,
    )


@router.get("/rules", response_model=list[RiskRuleInfo])
async def list_rules() -> list[RiskRuleInfo]:
    """列出风控规则."""
    return list(_mock_rules.values())


@router.get("/check", response_model=RiskCheckResponse)
async def check_rules(
    symbol: str = Query(..., description="标的代码"),
    order_type: str = Query("BUY", description="订单类型 BUY/SELL"),
    quantity: int = Query(0, description="数量"),
) -> RiskCheckResponse:
    """执行风控检查."""
    violations = []
    if quantity > 10000:
        violations.append({"rule": "MaxPositionSize", "message": "Order quantity exceeds max position size"})
    return RiskCheckResponse(passed=len(violations) == 0, violations=violations)


@router.post("/rules/{rule_id}/enable", response_model=RiskRuleInfo)
async def enable_rule(rule_id: str) -> RiskRuleInfo:
    """启用风控规则."""
    if rule_id not in _mock_rules:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Rule not found")
    _mock_rules[rule_id].enabled = True
    return _mock_rules[rule_id]


@router.post("/rules/{rule_id}/disable", response_model=RiskRuleInfo)
async def disable_rule(rule_id: str) -> RiskRuleInfo:
    """禁用风控规则."""
    if rule_id not in _mock_rules:
        raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Rule not found")
    _mock_rules[rule_id].enabled = False
    return _mock_rules[rule_id]


@router.get("/alerts")
async def get_risk_alerts(
    severity: str = Query("", description="告警级别 high/medium/low"),
    limit: int = Query(50, description="返回条数", le=200),
) -> dict:
    """风控告警列表."""
    return {"alerts": [], "total": 0}
