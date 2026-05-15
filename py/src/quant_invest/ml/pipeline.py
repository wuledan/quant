#!/usr/bin/env python3
"""ML训练管线."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date

import pandas as pd


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
    """ML训练管线."""

    def __init__(self, feature_builder=None, model=None, evaluator=None, versioning=None) -> None:
        self.feature_builder = feature_builder
        self.model = model
        self.evaluator = evaluator
        self.versioning = versioning

    def train(self, symbols: list[str], config: TrainConfig) -> dict:
        """执行模型训练."""
        return {"metrics": {}, "version": None, "model": self.model}

    def predict(self, symbols: list[str], date_: date, version: str | None = None) -> pd.Series:
        """使用指定版本模型预测."""
        return pd.Series(dtype=float)
