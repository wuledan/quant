#!/usr/bin/env python3
"""策略参数优化框架

支持网格搜索、随机搜索、贝叶斯优化、Walk-forward分析。
"""

from __future__ import annotations

import itertools
import random
import time
from dataclasses import dataclass
from datetime import date, timedelta
from typing import Any, Callable

import numpy as np
import pandas as pd


@dataclass
class ParamRange:
    """参数搜索范围"""

    name: str
    low: float
    high: float
    step: float | None = None
    values: list | None = None


@dataclass
class OptimizationResult:
    """优化结果"""

    best_params: dict
    best_score: float
    all_results: pd.DataFrame
    optimization_metric: str


class StrategyOptimizer:
    """策略参数优化框架

    支持优化方法：
    1. 网格搜索（Grid Search）
    2. 随机搜索（Random Search）
    3. 贝叶斯优化（Optuna）
    4. Walk-forward分析
    """

    def __init__(
        self,
        strategy_class: type | None = None,
        evaluate_fn: Callable | None = None,
        metric: str = "sharpe_ratio",
        method: str = "grid",
    ) -> None:
        self.strategy_class = strategy_class
        self.evaluate_fn = evaluate_fn
        self.metric = metric
        self.method = method

    def optimize(
        self,
        param_ranges: list[ParamRange],
        start_date: date,
        end_date: date,
        symbols: list[str],
        method: str | None = None,
        n_trials: int = 100,
        split_ratio: float = 0.7,
    ) -> OptimizationResult:
        """执行参数优化

        Args:
            param_ranges: 参数搜索范围列表
            start_date: 回测开始日期
            end_date: 回测结束日期
            symbols: 交易标的列表
            method: 搜索方法（grid / random / bayesian）
            n_trials: 随机/贝叶斯搜索的试验次数
            split_ratio: 训练集比例（用于防过拟合）

        Returns:
            OptimizationResult 包含最优参数和所有结果
        """
        method = method or self.method

        # 训练/测试分割
        total_days = (end_date - start_date).days
        train_end = start_date + timedelta(days=int(total_days * split_ratio))

        if method == "grid":
            return self._grid_search(param_ranges, start_date, end_date, symbols, train_end)
        elif method == "random":
            return self._random_search(param_ranges, start_date, end_date, symbols, n_trials, train_end)
        elif method == "bayesian":
            return self._bayesian_search(param_ranges, start_date, end_date, symbols, n_trials, train_end)
        else:
            raise ValueError(f"Unknown method: {method}. Choose from grid/random/bayesian")

    def walk_forward_analysis(
        self,
        param_ranges: list[ParamRange],
        train_period: int,
        test_period: int,
        start_date: date,
        end_date: date,
        symbols: list[str],
        method: str = "grid",
        n_trials: int = 50,
    ) -> list[OptimizationResult]:
        """Walk-forward分析

        滑动窗口方式：每个窗口内训练→测试，累积样本外表现。

        Args:
            param_ranges: 参数搜索范围
            train_period: 训练窗口长度（交易日数）
            test_period: 测试窗口长度（交易日数）
            start_date: 起始日期
            end_date: 结束日期
            symbols: 交易标的
            method: 搜索方法
            n_trials: 搜索试验次数

        Returns:
            每个窗口的OptimizationResult列表
        """
        trade_dates = pd.bdate_range(start_date, end_date).tolist()
        if len(trade_dates) < train_period + test_period:
            raise ValueError(
                f"Not enough data: need {train_period + test_period} days, "
                f"have {len(trade_dates)}"
            )

        results: list[OptimizationResult] = []
        window_start = 0
        while window_start + train_period + test_period <= len(trade_dates):
            train_start = trade_dates[window_start].date() if hasattr(trade_dates[window_start], 'date') else trade_dates[window_start]
            train_end = trade_dates[window_start + train_period - 1].date() if hasattr(trade_dates[window_start + train_period - 1], 'date') else trade_dates[window_start + train_period - 1]
            test_start = trade_dates[window_start + train_period].date() if hasattr(trade_dates[window_start + train_period], 'date') else trade_dates[window_start + train_period]
            test_end = trade_dates[min(window_start + train_period + test_period - 1, len(trade_dates) - 1)].date() if hasattr(trade_dates[min(window_start + train_period + test_period - 1, len(trade_dates) - 1)], 'date') else trade_dates[min(window_start + train_period + test_period - 1, len(trade_dates) - 1)]

            result = self.optimize(
                param_ranges=param_ranges,
                start_date=train_start,
                end_date=train_end,
                symbols=symbols,
                method=method,
                n_trials=n_trials,
                split_ratio=0.8,
            )

            result.all_results["window"] = window_start
            result.all_results["train_start"] = str(train_start)
            result.all_results["train_end"] = str(train_end)
            result.all_results["test_start"] = str(test_start)
            result.all_results["test_end"] = str(test_end)

            results.append(result)
            window_start += test_period

        return results

    # ── 搜索方法 ──────────────────────────────────────────────

    def _grid_search(
        self,
        param_ranges: list[ParamRange],
        start_date: date,
        end_date: date,
        symbols: list[str],
        train_end: date,
    ) -> OptimizationResult:
        """网格搜索：全排列遍历。"""
        param_grid = self._build_grid(param_ranges)
        all_results: list[dict] = []

        for combo in param_grid:
            score, test_score = self._evaluate_params(combo, start_date, end_date, symbols, train_end)
            row = combo.copy()
            row["train_score"] = score
            row["test_score"] = test_score
            all_results.append(row)

        df = pd.DataFrame(all_results)
        best_idx = df["train_score"].idxmax()
        best_params = {k: df.iloc[best_idx][k] for k in [r.name for r in param_ranges]}

        return OptimizationResult(
            best_params=best_params,
            best_score=df.iloc[best_idx]["train_score"],
            all_results=df,
            optimization_metric=self.metric,
        )

    def _random_search(
        self,
        param_ranges: list[ParamRange],
        start_date: date,
        end_date: date,
        symbols: list[str],
        n_trials: int,
        train_end: date,
    ) -> OptimizationResult:
        """随机搜索：随机采样n_trials次。"""
        all_results: list[dict] = []

        for _ in range(n_trials):
            combo = self._sample_random(param_ranges)
            score, test_score = self._evaluate_params(combo, start_date, end_date, symbols, train_end)
            row = combo.copy()
            row["train_score"] = score
            row["test_score"] = test_score
            all_results.append(row)

        df = pd.DataFrame(all_results)
        best_idx = df["train_score"].idxmax()
        best_params = {k: df.iloc[best_idx][k] for k in [r.name for r in param_ranges]}

        return OptimizationResult(
            best_params=best_params,
            best_score=df.iloc[best_idx]["train_score"],
            all_results=df,
            optimization_metric=self.metric,
        )

    def _bayesian_search(
        self,
        param_ranges: list[ParamRange],
        start_date: date,
        end_date: date,
        symbols: list[str],
        n_trials: int,
        train_end: date,
    ) -> OptimizationResult:
        """贝叶斯搜索：集成Optuna TPE采样器。"""
        try:
            import optuna
        except ImportError:
            raise ImportError(
                "optuna is required for bayesian_search. "
                "Install it with: pip install optuna"
            )

        def objective(trial: optuna.Trial) -> float:
            combo: dict[str, Any] = {}
            for r in param_ranges:
                if r.values is not None:
                    combo[r.name] = trial.suggest_categorical(r.name, r.values)
                elif r.step is not None:
                    n_steps = int((r.high - r.low) / r.step) + 1
                    combo[r.name] = trial.suggest_int(r.name, 0, n_steps - 1) * r.step + r.low
                else:
                    combo[r.name] = trial.suggest_float(r.name, r.low, r.high)

            score, test_score = self._evaluate_params(combo, start_date, end_date, symbols, train_end)
            trial.set_user_attr("test_score", test_score)
            return score

        study = optuna.create_study(direction="maximize", sampler=optuna.samplers.TPESampler())
        study.optimize(objective, n_trials=n_trials)

        trials_data = []
        for t in study.trials:
            if t.values is not None:
                row = {}
                for r in param_ranges:
                    row[r.name] = t.params.get(r.name)
                row["train_score"] = t.values[0]
                row["test_score"] = t.user_attrs.get("test_score", float("nan"))
                trials_data.append(row)

        df = pd.DataFrame(trials_data) if trials_data else pd.DataFrame()

        return OptimizationResult(
            best_params=study.best_params,
            best_score=study.best_value,
            all_results=df,
            optimization_metric=self.metric,
        )

    # ── 辅助方法 ──────────────────────────────────────────────

    def _build_grid(self, param_ranges: list[ParamRange]) -> list[dict[str, Any]]:
        """构建参数网格。"""
        all_values: list[list[tuple[str, Any]]] = []
        for r in param_ranges:
            if r.values is not None:
                values = [(r.name, v) for v in r.values]
            elif r.step is not None:
                vals = np.arange(r.low, r.high + r.step * 0.5, r.step)
                values = [(r.name, float(v)) for v in vals]
            else:
                val = (r.low + r.high) / 2
                values = [(r.name, val)]
            all_values.append(values)

        return [dict(combo) for combo in itertools.product(*all_values)]

    def _sample_random(self, param_ranges: list[ParamRange]) -> dict[str, Any]:
        """随机采样一组参数。"""
        combo: dict[str, Any] = {}
        for r in param_ranges:
            if r.values is not None:
                combo[r.name] = random.choice(r.values)
            elif r.step is not None:
                n_steps = int((r.high - r.low) / r.step) + 1
                combo[r.name] = random.randint(0, n_steps - 1) * r.step + r.low
            else:
                combo[r.name] = random.uniform(r.low, r.high)
        return combo

    def _evaluate_params(
        self,
        params: dict[str, Any],
        start_date: date,
        end_date: date,
        symbols: list[str],
        train_end: date,
    ) -> tuple[float, float]:
        """评估一组参数，返回(train_score, test_score)。

        如果提供了evaluate_fn，则调用它；否则使用模拟评分。
        训练集评分用于选择最优参数，测试集评分用于防过拟合验证。
        """
        if self.evaluate_fn is not None:
            train_score = self._call_evaluate(params, start_date, train_end, symbols)
            test_score = self._call_evaluate(params, train_end, end_date, symbols)
            return train_score, test_score
        return 0.0, 0.0

    def _call_evaluate(
        self,
        params: dict[str, Any],
        start: date,
        end: date,
        symbols: list[str],
    ) -> float:
        """调用评估函数。"""
        if self.evaluate_fn is None:
            return 0.0
        try:
            result = self.evaluate_fn(params=params, start_date=start, end_date=end, symbols=symbols)
            if isinstance(result, dict):
                return float(result.get(self.metric, 0.0))
            return float(result)
        except Exception:
            return float("-inf")
