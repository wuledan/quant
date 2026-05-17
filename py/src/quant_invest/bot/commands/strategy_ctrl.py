#!/usr/bin/env python3
"""策略控制指令处理器.

格式:
/策略 → 策略列表
/策略 启动 策略名 → 启动策略
/策略 停止 策略名 → 停止策略
/策略 参数 策略名 → 查看参数
/策略 状态 → 全局策略状态
"""

from __future__ import annotations

from .base import CommandHandler

# ── 模拟策略数据 ──

_mock_strategies: dict[str, dict] = {
    "动量策略": {
        "status": "running",
        "type": "momentum",
        "params": {"lookback": 20, "top_n": 10, "rebalance_days": 5},
        "started_at": "2024-06-01 09:30:00",
        "daily_pnl": 1250.0,
        "total_pnl": 15_300.0,
        "win_rate": 0.62,
    },
    "均线交叉": {
        "status": "stopped",
        "type": "ma_cross",
        "params": {"fast_period": 5, "slow_period": 20, "stop_loss": 0.02},
        "started_at": None,
        "daily_pnl": 0,
        "total_pnl": 8_200.0,
        "win_rate": 0.55,
    },
    "多因子选股": {
        "status": "running",
        "type": "multi_factor",
        "params": {"factors": ["momentum", "value", "quality"], "rebalance_days": 10},
        "started_at": "2024-05-15 09:30:00",
        "daily_pnl": 860.0,
        "total_pnl": 22_100.0,
        "win_rate": 0.58,
    },
}


class StrategyCommand(CommandHandler):
    """策略控制指令."""

    async def handle(self, text: str, event: dict) -> dict:
        """处理策略指令."""
        parts = text.strip().split()
        if len(parts) <= 1:
            return self._handle_list()

        action = parts[1]

        if action == "启动":
            name = parts[2] if len(parts) > 2 else ""
            return self._handle_start(name)
        elif action == "停止":
            name = parts[2] if len(parts) > 2 else ""
            return self._handle_stop(name)
        elif action == "参数":
            name = parts[2] if len(parts) > 2 else ""
            return self._handle_params(name)
        elif action == "状态":
            return self._handle_status()
        else:
            return {"success": False, "error": f"未知操作: {action}。支持: 启动/停止/参数/状态"}

    def _handle_list(self) -> dict:
        """策略列表."""
        lines = ["📋 策略列表"]
        for name, s in _mock_strategies.items():
            status_icon = "🟢" if s["status"] == "running" else "🔴"
            lines.append(
                f"{status_icon} {name} ({s['type']}) - "
                f"累计盈亏: ¥{s['total_pnl']:,.0f} 胜率: {s['win_rate']:.0%}"
            )
        return {"success": True, "message": "\n".join(lines)}

    def _handle_start(self, name: str) -> dict:
        """启动策略."""
        if not name:
            return {"success": False, "error": "请指定策略名称"}
        if name not in _mock_strategies:
            return {"success": False, "error": f"策略 '{name}' 不存在"}
        s = _mock_strategies[name]
        if s["status"] == "running":
            return {"success": False, "error": f"策略 '{name}' 已在运行中"}
        s["status"] = "running"
        return {"success": True, "message": f"✅ 策略 '{name}' 已启动"}

    def _handle_stop(self, name: str) -> dict:
        """停止策略."""
        if not name:
            return {"success": False, "error": "请指定策略名称"}
        if name not in _mock_strategies:
            return {"success": False, "error": f"策略 '{name}' 不存在"}
        s = _mock_strategies[name]
        if s["status"] == "stopped":
            return {"success": False, "error": f"策略 '{name}' 已停止"}
        s["status"] = "stopped"
        return {"success": True, "message": f"⏹ 策略 '{name}' 已停止"}

    def _handle_params(self, name: str) -> dict:
        """查看策略参数."""
        if not name:
            return {"success": False, "error": "请指定策略名称"}
        if name not in _mock_strategies:
            return {"success": False, "error": f"策略 '{name}' 不存在"}
        s = _mock_strategies[name]
        params_str = "\n".join(f"  {k}: {v}" for k, v in s["params"].items())
        msg = f"⚙️ 策略 '{name}' 参数:\n{params_str}"
        return {"success": True, "message": msg, "params": s["params"]}

    def _handle_status(self) -> dict:
        """全局策略状态."""
        running = sum(1 for s in _mock_strategies.values() if s["status"] == "running")
        stopped = len(_mock_strategies) - running
        total_pnl = sum(s["daily_pnl"] for s in _mock_strategies.values())

        msg = (
            f"📊 策略全局状态\n"
            f"运行中: {running} | 已停止: {stopped}\n"
            f"当日总盈亏: ¥{total_pnl:,.0f}"
        )
        return {"success": True, "message": msg}
