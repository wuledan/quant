#!/usr/bin/env python3
"""API路由测试."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from quant_invest.api.app import create_app


@pytest.fixture
def client():
    """创建测试客户端."""
    app = create_app()
    return TestClient(app)


class TestHealthCheck:
    """健康检查测试."""

    def test_health(self, client):
        """健康检查."""
        resp = client.get("/health")
        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "ok"


class TestDataRouter:
    """数据查询路由测试."""

    def test_get_kline(self, client):
        """获取K线数据."""
        resp = client.get("/api/v1/data/kline", params={"symbol": "000001.SZ"})
        assert resp.status_code == 200
        data = resp.json()
        assert data["symbol"] == "000001.SZ"
        assert "data" in data

    def test_get_kline_with_params(self, client):
        """K线数据带参数."""
        resp = client.get(
            "/api/v1/data/kline",
            params={
                "symbol": "000001.SZ",
                "frequency": "weekly",
                "start_date": "2024-01-01",
                "end_date": "2024-12-31",
            },
        )
        assert resp.status_code == 200

    def test_get_kline_missing_symbol(self, client):
        """K线缺少必填参数."""
        resp = client.get("/api/v1/data/kline")
        assert resp.status_code == 422

    def test_get_tick(self, client):
        """获取Tick数据."""
        resp = client.get("/api/v1/data/tick", params={"symbol": "000001.SZ", "date": "2024-01-02"})
        assert resp.status_code == 200
        data = resp.json()
        assert data["symbol"] == "000001.SZ"

    def test_get_financial(self, client):
        """获取财务数据."""
        resp = client.get("/api/v1/data/financial", params={"symbol": "000001.SZ"})
        assert resp.status_code == 200
        data = resp.json()
        assert data["symbol"] == "000001.SZ"

    def test_get_trade_calendar(self, client):
        """获取交易日历."""
        resp = client.get(
            "/api/v1/data/trade_calendar",
            params={"start_date": "2024-01-01", "end_date": "2024-12-31"},
        )
        assert resp.status_code == 200

    def test_get_index_constituents(self, client):
        """获取指数成分股."""
        resp = client.get("/api/v1/data/index_constituents", params={"index_code": "000300"})
        assert resp.status_code == 200


class TestStrategyRouter:
    """策略管理路由测试."""

    def test_list_strategies(self, client):
        """策略列表."""
        resp = client.get("/api/v1/strategy/list")
        assert resp.status_code == 200

    def test_get_strategy(self, client):
        """策略详情."""
        resp = client.get("/api/v1/strategy/test-strategy-1")
        assert resp.status_code == 200
        data = resp.json()
        assert data["strategy_id"] == "test-strategy-1"

    def test_start_strategy(self, client):
        """启动策略."""
        resp = client.post("/api/v1/strategy/start", params={"strategy_id": "test-strategy-1"})
        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "running"

    def test_stop_strategy(self, client):
        """停止策略."""
        resp = client.post("/api/v1/strategy/stop", params={"strategy_id": "test-strategy-1"})
        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "stopped"


class TestBacktestRouter:
    """回测路由测试."""

    def test_run_backtest(self, client):
        """提交回测."""
        resp = client.post(
            "/api/v1/backtest/run",
            json={
                "strategy_id": "test-strategy",
                "symbols": ["000001.SZ"],
                "start_date": "2024-01-01",
                "end_date": "2024-12-31",
            },
        )
        assert resp.status_code == 200
        data = resp.json()
        assert "backtest_id" in data

    def test_get_backtest_result(self, client):
        """查询回测结果."""
        resp = client.get("/api/v1/backtest/result/bt-001")
        assert resp.status_code == 200
        data = resp.json()
        assert data["backtest_id"] == "bt-001"

    def test_compare_backtests(self, client):
        """对比回测."""
        resp = client.get("/api/v1/backtest/compare", params={"backtest_ids": "bt-001,bt-002"})
        assert resp.status_code == 200
        data = resp.json()
        assert len(data["backtest_ids"]) == 2

    def test_list_backtests(self, client):
        """回测列表."""
        resp = client.get("/api/v1/backtest/list")
        assert resp.status_code == 200


class TestPortfolioRouter:
    """持仓组合路由测试."""

    def test_get_snapshot(self, client):
        """持仓快照."""
        resp = client.get("/api/v1/portfolio/snapshot")
        assert resp.status_code == 200

    def test_get_history(self, client):
        """持仓历史."""
        resp = client.get("/api/v1/portfolio/history")
        assert resp.status_code == 200

    def test_get_performance(self, client):
        """绩效分析."""
        resp = client.get("/api/v1/portfolio/performance")
        assert resp.status_code == 200


class TestFactorRouter:
    """因子路由测试."""

    def test_calculate_factor(self, client):
        """因子计算."""
        resp = client.post(
            "/api/v1/factor/calculate",
            json={"factor_names": ["momentum_20"], "symbols": ["000001.SZ"], "date": "2024-01-02"},
        )
        assert resp.status_code == 200

    def test_list_factors(self, client):
        """因子列表."""
        resp = client.get("/api/v1/factor/list")
        assert resp.status_code == 200

    def test_get_factor_info(self, client):
        """因子详情."""
        resp = client.get("/api/v1/factor/momentum_20/info")
        assert resp.status_code == 200


class TestRiskRouter:
    """风控路由测试."""

    def test_get_risk_status(self, client):
        """风控状态."""
        resp = client.get("/api/v1/risk/status")
        assert resp.status_code == 200
        data = resp.json()
        assert data["overall_status"] in ("normal", "warning", "danger")

    def test_get_risk_alerts(self, client):
        """风控告警列表."""
        resp = client.get("/api/v1/risk/alerts")
        assert resp.status_code == 200

    def test_get_risk_alerts_with_filter(self, client):
        """风控告警带过滤."""
        resp = client.get("/api/v1/risk/alerts", params={"severity": "high", "limit": 10})
        assert resp.status_code == 200

    def test_check_rules(self, client):
        """检查风控规则."""
        resp = client.post(
            "/api/v1/risk/rules/check",
            params={"symbol": "000001.SZ", "order_type": "BUY", "quantity": 1000},
        )
        assert resp.status_code == 200
        data = resp.json()
        assert "passed" in data


class TestSystemRouter:
    """系统路由测试."""

    def test_health(self, client):
        """系统健康."""
        resp = client.get("/api/v1/system/health")
        assert resp.status_code == 200
        data = resp.json()
        assert data["status"] == "ok"

    def test_system_status(self, client):
        """系统状态."""
        resp = client.get("/api/v1/system/status")
        assert resp.status_code == 200


class TestWebSocketManager:
    """WebSocket管理器测试."""

    def test_connect_and_broadcast(self):
        """测试WebSocket管理器基本功能."""
        from quant_invest.api.ws import WebSocketManager

        manager = WebSocketManager()
        assert len(manager._connections) == 0

    def test_broadcast_empty_channel(self):
        """空频道广播不报错."""
        from quant_invest.api.ws import WebSocketManager

        manager = WebSocketManager()
        import asyncio

        asyncio.get_event_loop().run_until_complete(manager.broadcast("market", {"type": "tick"}))

    def test_close_empty(self):
        """关闭空管理器不报错."""
        from quant_invest.api.ws import WebSocketManager

        manager = WebSocketManager()
        import asyncio

        asyncio.get_event_loop().run_until_complete(manager.close())