#!/usr/bin/env python3
"""ML训练管线."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date

import numpy as np
import pandas as pd

from .evaluation import EvaluationMetrics, ModelEvaluator
from .versioning import ModelVersioning


@dataclass
class TrainConfig:
    """训练配置."""

    train_start: date
    train_end: date
    valid_start: date
    valid_end: date
    test_start: date
    test_end: date
    retrain_interval: int = 20
    walk_forward: bool = True


class MLPipeline:
    """ML训练管线.

    支持的训练流程：
    1. 特征构建 → 时序切分
    2. 模型训练（支持自定义模型对象）
    3. 模型评估 → 版本注册
    """

    def __init__(
        self,
        feature_builder=None,
        model=None,
        evaluator: ModelEvaluator | None = None,
        versioning: ModelVersioning | None = None,
    ) -> None:
        self.feature_builder = feature_builder
        self.model = model
        self.evaluator = evaluator or ModelEvaluator()
        self.versioning = versioning or ModelVersioning()

    def train(self, symbols: list[str], config: TrainConfig) -> dict:
        """执行模型训练.

        Returns:
            dict with keys: metrics, version, model
        """
        if self.feature_builder is None or self.model is None:
            return {"metrics": {}, "version": None, "model": self.model}

        # 构建特征
        X, y = self.feature_builder.build(
            symbols, config.train_start, config.test_end
        )

        if X.empty:
            return {"metrics": {}, "version": None, "model": self.model}

        # 时序切分
        X_train, y_train, X_valid, y_valid, X_test, y_test = (
            self.feature_builder.split_time_series(X, y)
        )

        # 训练模型
        self.model.fit(X_train, y_train)

        # 评估
        metrics = self.evaluator.evaluate(self.model, X_test, y_test)

        # 计算IC
        predictions = self.model.predict(X_test)
        ic_series = self._calc_ic_series(predictions, y_test)

        metrics_dict = {
            "mse": metrics.mse,
            "rmse": metrics.rmse,
            "mae": metrics.mae,
            "r2": metrics.r2,
            "rank_ic": metrics.rank_ic,
            "ic_mean": float(ic_series.mean()) if len(ic_series) > 0 else 0.0,
            "ic_ir": float(ic_series.mean() / ic_series.std()) if len(ic_series) > 1 and ic_series.std() > 0 else 0.0,
        }

        # 注册版本
        version = self.versioning.register(
            model=self.model,
            metrics=metrics_dict,
            config={"model_type": getattr(self.model, "model_type", "unknown")},
            feature_list=self.feature_builder.feature_names,
        )

        return {"metrics": metrics_dict, "version": version, "model": self.model}

    def predict(
        self, symbols: list[str], date_: date, version: str | None = None
    ) -> pd.Series:
        """使用指定版本模型预测."""
        if self.model is None:
            return pd.Series(dtype=float)

        if version and self.versioning:
            prod = self.versioning.get_production(
                getattr(self.model, "model_type", "unknown")
            )
            if prod:
                # 使用生产版本（此处简化，实际需加载模型文件）
                pass

        return self.model.predict(pd.DataFrame())

    @staticmethod
    def _calc_ic_series(predictions, actual) -> pd.Series:
        """计算IC时序序列."""
        if not hasattr(predictions, "index") or not hasattr(actual, "index"):
            return pd.Series(dtype=float)

        if isinstance(predictions.index, pd.MultiIndex):
            dates = predictions.index.get_level_values("date").unique()
            ic_values = []
            for d in dates:
                pred_d = predictions.xs(d, level="date")
                actual_d = actual.xs(d, level="date")
                if len(pred_d) > 2:
                    ic = pred_d.corr(actual_d, method="spearman")
                    ic_values.append(ic)
                else:
                    ic_values.append(0.0)
            return pd.Series(ic_values, index=dates)
        return pd.Series(dtype=float)