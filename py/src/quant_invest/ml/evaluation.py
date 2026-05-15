#!/usr/bin/env python3
"""模型评估器."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pandas as pd


@dataclass
class EvaluationMetrics:
    """模型评估指标."""

    mse: float = 0.0
    rmse: float = 0.0
    mae: float = 0.0
    r2: float = 0.0
    rank_ic: float = 0.0
    rank_ic_ir: float = 0.0
    top_decile_return: float = 0.0
    monotonicity: float = 0.0
    accuracy: float | None = None
    precision: float | None = None
    recall: float | None = None
    auc: float | None = None


class ModelEvaluator:
    """模型评估器."""

    def evaluate(self, model, X_test: pd.DataFrame, y_test: pd.Series) -> EvaluationMetrics:
        """综合评估模型."""
        predictions = model.predict(X_test)
        mse = ((predictions - y_test) ** 2).mean()
        metrics = EvaluationMetrics(
            mse=mse,
            rmse=np.sqrt(mse),
            mae=np.abs(predictions - y_test).mean(),
        )
        ss_res = ((y_test - predictions) ** 2).sum()
        ss_tot = ((y_test - y_test.mean()) ** 2).sum()
        metrics.r2 = 1 - ss_res / ss_tot if ss_tot > 0 else 0.0
        return metrics

    @staticmethod
    def calc_rank_ic(predictions: pd.Series, actual: pd.Series) -> float:
        """计算Rank IC."""
        return predictions.corr(actual, method="spearman")
