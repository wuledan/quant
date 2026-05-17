#!/usr/bin/env python3
"""StrategyOptimizer单元测试."""

from __future__ import annotations

from datetime import date

import pandas as pd
import pytest

from quant_invest.strategy.optimizer import OptimizationResult, ParamRange, StrategyOptimizer


def dummy_evaluate(params: dict, start_date: date, end_date: date, symbols: list[str]) -> dict:
    """模拟评估函数."""
    ma_fast = params.get("ma_fast", 10)
    ma_slow = params.get("ma_slow", 30)
    stop_loss = params.get("stop_loss", 0.02)
    sharpe = (ma_slow - ma_fast) / 100.0 + stop_loss * 10
    return {"sharpe_ratio": sharpe}


class TestStrategyOptimizer:
    """StrategyOptimizer 单元测试."""

    @pytest.fixture
    def optimizer(self) -> StrategyOptimizer:
        return StrategyOptimizer(evaluate_fn=dummy_evaluate, metric="sharpe_ratio")

    @pytest.fixture
    def param_ranges(self) -> list[ParamRange]:
        return [
            ParamRange(name="ma_fast", low=5, high=20, step=5),
            ParamRange(name="ma_slow", low=20, high=60, step=10),
            ParamRange(name="stop_loss", low=0.01, high=0.05, step=0.01),
        ]

    def test_grid_search(self, optimizer, param_ranges):
        result = optimizer.optimize(
            param_ranges=param_ranges,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="grid",
        )
        assert isinstance(result, OptimizationResult)
        assert len(result.best_params) == 3
        assert isinstance(result.best_score, float)
        assert len(result.all_results) > 0

    def test_grid_search_all_combinations(self, optimizer, param_ranges):
        result = optimizer.optimize(
            param_ranges=param_ranges,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="grid",
        )
        total = 4 * 5 * 5
        assert len(result.all_results) == total

    def test_random_search(self, optimizer, param_ranges):
        result = optimizer.optimize(
            param_ranges=param_ranges,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="random",
            n_trials=20,
        )
        assert len(result.all_results) == 20
        assert len(result.best_params) == 3

    def test_random_search_with_discrete_values(self, optimizer):
        ranges = [
            ParamRange(name="ma_fast", low=0, high=0, values=[5, 10, 20]),
        ]
        result = optimizer.optimize(
            param_ranges=ranges,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="random",
            n_trials=10,
        )
        assert len(result.all_results) == 10

    def test_bayesian_search(self, optimizer, param_ranges):
        pytest.importorskip("optuna")
        result = optimizer.optimize(
            param_ranges=param_ranges,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="bayesian",
            n_trials=10,
        )
        assert len(result.best_params) == 3
        assert result.best_score != float("-inf")

    def test_unknown_method(self, optimizer, param_ranges):
        with pytest.raises(ValueError, match="Unknown method"):
            optimizer.optimize(
                param_ranges=param_ranges,
                start_date=date(2024, 1, 1),
                end_date=date(2024, 6, 30),
                symbols=["000001.SZ"],
                method="unknown",
            )

    def test_empty_param_ranges(self, optimizer):
        result = optimizer.optimize(
            param_ranges=[],
            start_date=date(2024, 1, 1),
            end_date=date(2024, 6, 30),
            symbols=["000001.SZ"],
            method="grid",
        )
        assert len(result.all_results) == 0

    def test_walk_forward_analysis(self, optimizer, param_ranges):
        results = optimizer.walk_forward_analysis(
            param_ranges=param_ranges,
            train_period=10,
            test_period=5,
            start_date=date(2024, 1, 1),
            end_date=date(2024, 2, 28),
            symbols=["000001.SZ"],
            method="grid",
        )
        assert len(results) >= 1
        for r in results:
            assert "window" in r.all_results.columns

    def test_walk_forward_insufficient_data(self, optimizer, param_ranges):
        with pytest.raises(ValueError, match="Not enough data"):
            optimizer.walk_forward_analysis(
                param_ranges=param_ranges,
                train_period=100,
                test_period=50,
                start_date=date(2024, 1, 1),
                end_date=date(2024, 1, 10),
                symbols=["000001.SZ"],
            )

    def test_train_test_split(self, optimizer, param_ranges):
        result = optimizer.optimize(
            param_ranges=[ParamRange(name="ma_fast", low=5, high=20, values=[5, 10, 20])],
            start_date=date(2024, 1, 1),
            end_date=date(2024, 3, 31),
            symbols=["000001.SZ"],
            method="grid",
            split_ratio=0.7,
        )
        assert "train_score" in result.all_results.columns
        assert "test_score" in result.all_results.columns
