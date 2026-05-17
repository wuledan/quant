#!/usr/bin/env python3
"""持仓查询指令处理器.

格式:
/持仓 → 当前组合概览
/持仓 明细 → 各标的持仓详情
/持仓 盈亏 → 盈亏分析
"""

from __future__ import annotations

from .base import CommandHandler

# ── 模拟持仓数据 ──

_mock_portfolio = {
    "total_value": 1_152_300.0,
    "cash": 200_000.0,
    "positions_value": 952_300.0,
    "daily_pnl": 850.0,
    "total_pnl": 15_2300.0,
    "positions": [
        {"symbol": "000001.SZ", "name": "平安银行", "qty": 1000, "cost": 10.20,
         "price": 10.50, "value": 10_500, "pnl": 300, "pnl_pct": 2.94},
        {"symbol": "600519.SH", "name": "贵州茅台", "qty": 100, "cost": 1800.0,
         "price": 1850.0, "value": 185_000, "pnl": 5000, "pnl_pct": 2.78},
        {"symbol": "000858.SZ", "name": "五粮液", "qty": 300, "cost": 145.0,
         "price": 152.0, "value": 45_600, "pnl": 2100, "pnl_pct": 4.83},
    ],
}


class PortfolioCommand(CommandHandler):
    """持仓查询指令."""

    async def handle(self, text: str, event: dict) -> dict:
        """处理持仓指令."""
        parts = text.strip().split()
        sub_cmd = parts[1] if len(parts) > 1 else ""

        if sub_cmd == "明细":
            return self._handle_detail()
        elif sub_cmd == "盈亏":
            return self._handle_pnl()
        else:
            return self._handle_summary()

    def _handle_summary(self) -> dict:
        """组合概览."""
        p = _mock_portfolio
        msg = (
            f"📊 组合概览\n"
            f"总资产: ¥{p['total_value']:,.0f}\n"
            f"持仓市值: ¥{p['positions_value']:,.0f}\n"
            f"可用现金: ¥{p['cash']:,.0f}\n"
            f"当日盈亏: ¥{p['daily_pnl']:,.0f}\n"
            f"累计盈亏: ¥{p['total_pnl']:,.0f}\n"
            f"持仓数: {len(p['positions'])}"
        )
        return {"success": True, "message": msg, "data": p}

    def _handle_detail(self) -> dict:
        """持仓明细."""
        p = _mock_portfolio
        lines = ["📋 持仓明细"]
        for pos in p["positions"]:
            pnl_sign = "+" if pos["pnl"] >= 0 else ""
            lines.append(
                f"• {pos['name']}({pos['symbol']}) "
                f"数量:{pos['qty']} 成本:{pos['cost']:.2f} "
                f"现价:{pos['price']:.2f} "
                f"盈亏:{pnl_sign}{pos['pnl']:,.0f}({pnl_sign}{pos['pnl_pct']:.1f}%)"
            )
        return {"success": True, "message": "\n".join(lines)}

    def _handle_pnl(self) -> dict:
        """盈亏分析."""
        p = _mock_portfolio
        lines = [
            "💰 盈亏分析",
            f"当日盈亏: ¥{p['daily_pnl']:,.0f}",
            f"累计盈亏: ¥{p['total_pnl']:,.0f}",
            "",
            "个股盈亏:",
        ]
        for pos in sorted(p["positions"], key=lambda x: x["pnl"], reverse=True):
            pnl_sign = "+" if pos["pnl"] >= 0 else ""
            lines.append(
                f"• {pos['name']}: {pnl_sign}¥{pos['pnl']:,.0f} ({pnl_sign}{pos['pnl_pct']:.1f}%)"
            )
        return {"success": True, "message": "\n".join(lines)}
