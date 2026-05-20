#!/usr/bin/env python3
"""回测任务管理器 — 常驻服务，接收任务提交，并发执行，支持进度回调."""

from __future__ import annotations

import logging
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import date, datetime, timezone
from enum import Enum
from typing import Any, Callable

from ..strategy.registry import StrategyRegistry

logger = logging.getLogger("quant_invest.backtest.manager")


class TaskStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"


@dataclass
class BacktestTaskInfo:
    """任务元信息."""
    task_id: str
    strategy_id: str
    strategy_type: str
    symbols: list[str]
    start_date: date
    end_date: date
    initial_cash: float
    status: TaskStatus = TaskStatus.PENDING
    progress: float = 0.0
    message: str = ""
    submitted_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    completed_at: datetime | None = None
    result: dict[str, Any] | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "task_id": self.task_id,
            "strategy_id": self.strategy_id,
            "status": self.status.value,
            "progress": self.progress,
            "message": self.message,
            "submitted_at": self.submitted_at.isoformat(),
            "completed_at": self.completed_at.isoformat() if self.completed_at else None,
            "symbols": self.symbols,
            "start_date": self.start_date.isoformat(),
            "end_date": self.end_date.isoformat(),
        }


class BacktestTaskManager:
    """回测任务管理器.

    常驻服务，提供：
    - 任务提交与排队
    - 线程池并发执行
    - 实时进度回调
    - 结果缓存
    """

    def __init__(self, max_workers: int = 2) -> None:
        self._executor = ThreadPoolExecutor(max_workers=max_workers, thread_name_prefix="backtest")
        self._tasks: dict[str, BacktestTaskInfo] = {}
        self._futures: dict[str, Future] = {}
        self._results: dict[str, dict[str, Any]] = {}
        self._progress_callbacks: list[Callable[[str, float, str], None]] = []

    def submit(
        self,
        strategy_id: str,
        strategy_type: str,
        symbols: list[str],
        start_date: date,
        end_date: date,
        initial_cash: float = 1_000_000.0,
        strategy_params: dict | None = None,
    ) -> str:
        """提交回测任务，返回task_id."""
        task_id = f"bt-{uuid.uuid4().hex[:8]}"
        task = BacktestTaskInfo(
            task_id=task_id,
            strategy_id=strategy_id,
            strategy_type=strategy_type,
            symbols=symbols,
            start_date=start_date,
            end_date=end_date,
            initial_cash=initial_cash,
        )
        self._tasks[task_id] = task
        logger.info("任务提交: %s, 策略=%s(%s), 标的=%s", task_id, strategy_id, strategy_type, symbols)

        future = self._executor.submit(self._execute, task, strategy_params or {})
        future.add_done_callback(lambda f: self._on_task_done(task_id, f))
        self._futures[task_id] = future
        return task_id

    def get_status(self, task_id: str) -> dict[str, Any] | None:
        """获取任务状态."""
        task = self._tasks.get(task_id)
        if task is None:
            return None
        return task.to_dict()

    def get_result(self, task_id: str) -> dict[str, Any] | None:
        """获取回测结果."""
        return self._results.get(task_id)

    def list_tasks(self, limit: int = 20) -> list[dict[str, Any]]:
        """列出所有任务."""
        tasks = sorted(self._tasks.values(), key=lambda t: t.submitted_at, reverse=True)
        return [t.to_dict() for t in tasks[:limit]]

    def add_progress_callback(self, cb: Callable[[str, float, str], None]) -> None:
        """注册进度回调."""
        self._progress_callbacks.append(cb)

    def shutdown(self) -> None:
        """关闭管理器."""
        self._executor.shutdown(wait=False)
        logger.info("回测任务管理器已关闭")

    # ── 内部执行 ──────────────────────────────────────────────

    def _execute(self, task: BacktestTaskInfo, strategy_params: dict) -> None:
        """在线程池中执行回测."""
        try:
            task.status = TaskStatus.RUNNING
            task.message = "正在准备数据..."
            self._notify(task.task_id, 0.0, "正在准备数据")

            # 1. 实例化策略
            strategy_cls = StrategyRegistry.get(task.strategy_type)
            strategy = strategy_cls(**strategy_params)

            # 2. 创建数据处理器（优先走调度器缓存）
            from ..data.providers import DataProviderFactory
            from .data_handler import DailyDataHandler

            provider = DataProviderFactory.create("yahoo")
            data_handler = DailyDataHandler(
                symbols=task.symbols,
                start_date=task.start_date,
                end_date=task.end_date,
                data_provider=provider,
            )

            if not data_handler._dates:
                raise ValueError(f"无可用数据: {task.symbols}, {task.start_date}~{task.end_date}")

            logger.info("数据加载完成: %d 根K线", len(data_handler._dates))
            task.message = f"数据加载完成，{len(data_handler._dates)}根K线"

            # 3. 创建引擎组件
            from .broker import SimulatedBroker
            from .portfolio import Portfolio

            portfolio = Portfolio()
            portfolio.initialize(task.initial_cash)
            broker = SimulatedBroker()

            # 4. 进度回调
            def on_progress(current: int, total: int, msg: str) -> None:
                pct = current / total * 100 if total > 0 else 0
                task.progress = round(pct, 1)
                task.message = msg
                self._notify(task.task_id, pct, msg)

            # 5. 运行回测
            from .engine import BacktestEngine

            engine = BacktestEngine(
                data_handler=data_handler,
                broker=broker,
                portfolio=portfolio,
                strategy=strategy,
                initial_cash=task.initial_cash,
                on_progress=on_progress,
            )

            result = engine.run(
                start_date=task.start_date,
                end_date=task.end_date,
            )

            # 6. 构建结果
            nav_curve = []
            if result.nav_series is not None:
                for dt, nav in result.nav_series.items():
                    nav_curve.append({
                        "date": dt.strftime("%Y-%m-%d") if hasattr(dt, "strftime") else str(dt),
                        "nav": round(float(nav), 2),
                    })

            trades = result.trades or []
            m = result.metrics

            result_dict = {
                "backtest_id": task.task_id,
                "status": "completed",
                "strategy_id": task.strategy_id,
                "metrics": {
                    "total_return": result.total_return,
                    "annual_return": getattr(m, "annual_return", 0) if m else 0,
                    "max_drawdown": getattr(m, "max_drawdown", 0) if m else 0,
                    "sharpe_ratio": getattr(m, "sharpe_ratio", 0) if m else 0,
                    "win_rate": getattr(m, "win_rate", 0) if m else 0,
                    "total_trades": len(trades),
                    "profit_factor": getattr(m, "profit_loss_ratio", 0) if m else 0,
                },
                "nav_curve": nav_curve,
                "trades": trades,
            }

            self._results[task.task_id] = result_dict
            task.result = result_dict
            task.status = TaskStatus.COMPLETED
            task.progress = 100.0
            task.message = "回测完成"
            task.completed_at = datetime.now(timezone.utc)

            logger.info("任务完成: %s, 收益=%.4f, 交易=%d", task.task_id, result.total_return, len(trades))

        except Exception as e:
            logger.exception("任务失败: %s", task.task_id)
            task.status = TaskStatus.FAILED
            task.message = f"回测失败: {e}"
            task.completed_at = datetime.now(timezone.utc)

    def _on_task_done(self, task_id: str, future: Future) -> None:
        """任务完成回调（不论成功失败）."""
        exc = future.exception()
        if exc:
            logger.error("任务 %s 异常: %s", task_id, exc)

    def _notify(self, task_id: str, progress: float, message: str) -> None:
        """触发进度回调."""
        for cb in self._progress_callbacks:
            try:
                cb(task_id, progress, message)
            except Exception:
                pass


# ── 全局单例 ──────────────────────────────────────────────

_manager: BacktestTaskManager | None = None


def get_backtest_manager() -> BacktestTaskManager:
    """获取全局回测任务管理器."""
    global _manager
    if _manager is None:
        _manager = BacktestTaskManager(max_workers=2)
    return _manager
