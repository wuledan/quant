#!/usr/bin/env python3
"""ML模型基类."""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass

import numpy as np
import pandas as pd


@dataclass
class ModelConfig:
    """模型配置基类."""

    model_type: str
    hyperparams: dict
    random_seed: int = 42


class ModelBase(ABC):
    """模型基类."""

    @abstractmethod
    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> ModelBase:
        """训练模型."""
        ...

    @abstractmethod
    def predict(self, X: pd.DataFrame) -> np.ndarray:
        """预测."""
        ...

    @abstractmethod
    def save(self, path: str) -> None:
        """保存模型."""
        ...

    @abstractmethod
    def load(self, path: str) -> None:
        """加载模型."""
        ...
