#!/usr/bin/env python3
"""XGBoost/LightGBM 模型实现."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd

from .base import ModelBase, ModelConfig


class XGBoostModel(ModelBase):
    """XGBoost 回归模型."""

    def __init__(self, config: ModelConfig | None = None) -> None:
        self.config = config or ModelConfig(
            model_type="xgboost",
            hyperparams={"n_estimators": 200, "max_depth": 6, "learning_rate": 0.05},
        )
        self._model = None

    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "XGBoostModel":
        xgb = __import__("xgboost")
        params = {**self.config.hyperparams, **kwargs}
        self._model = xgb.XGBRegressor(
            n_estimators=params.get("n_estimators", 200),
            max_depth=params.get("max_depth", 6),
            learning_rate=params.get("learning_rate", 0.05),
            random_state=self.config.random_seed,
            verbosity=0,
        )
        self._model.fit(X, y)
        return self

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        if self._model is None:
            raise RuntimeError("Model not trained")
        return self._model.predict(X)

    def save(self, path: str) -> None:
        if self._model is None:
            raise RuntimeError("Model not trained")
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        self._model.save_model(str(p.with_suffix(".json")))
        meta = {"model_type": self.config.model_type, "hyperparams": self.config.hyperparams}
        p.with_suffix(".meta.json").write_text(json.dumps(meta))

    def load(self, path: str) -> None:
        xgb = __import__("xgboost")
        p = Path(path)
        meta_path = p.with_suffix(".meta.json")
        if meta_path.exists():
            meta = json.loads(meta_path.read_text())
            self.config.hyperparams = meta.get("hyperparams", self.config.hyperparams)
        self._model = xgb.XGBRegressor()
        self._model.load_model(str(p.with_suffix(".json")))

    @property
    def feature_importances(self) -> dict[str, float]:
        if self._model is None:
            return {}
        return dict(sorted(
            zip(self._model.get_booster().feature_names or [],
                self._model.feature_importances_.tolist()),
            key=lambda x: x[1], reverse=True,
        ))


class LightGBMModel(ModelBase):
    """LightGBM 回归模型."""

    def __init__(self, config: ModelConfig | None = None) -> None:
        self.config = config or ModelConfig(
            model_type="lightgbm",
            hyperparams={"n_estimators": 200, "max_depth": 6, "learning_rate": 0.05, "num_leaves": 31},
        )
        self._model = None

    def fit(self, X: pd.DataFrame, y: pd.Series, **kwargs) -> "LightGBMModel":
        lgb = __import__("lightgbm")
        params = {**self.config.hyperparams, **kwargs}
        self._model = lgb.LGBMRegressor(
            n_estimators=params.get("n_estimators", 200),
            max_depth=params.get("max_depth", 6),
            learning_rate=params.get("learning_rate", 0.05),
            num_leaves=params.get("num_leaves", 31),
            random_state=self.config.random_seed,
            verbose=-1,
        )
        self._model.fit(X, y)
        return self

    def predict(self, X: pd.DataFrame) -> np.ndarray:
        if self._model is None:
            raise RuntimeError("Model not trained")
        return self._model.predict(X)

    def save(self, path: str) -> None:
        if self._model is None:
            raise RuntimeError("Model not trained")
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        self._model.booster_.save_model(str(p.with_suffix(".txt")))
        meta = {"model_type": self.config.model_type, "hyperparams": self.config.hyperparams}
        p.with_suffix(".meta.json").write_text(json.dumps(meta))

    def load(self, path: str) -> None:
        lgb = __import__("lightgbm")
        p = Path(path)
        meta_path = p.with_suffix(".meta.json")
        if meta_path.exists():
            meta = json.loads(meta_path.read_text())
            self.config.hyperparams = meta.get("hyperparams", self.config.hyperparams)
        self._model = lgb.Booster(model_file=str(p.with_suffix(".txt")))
        # Wrap for sklearn-like predict
        booster = self._model
        self._model = lgb.LGBMRegressor()
        self._model._Booster = booster

    @property
    def feature_importances(self) -> dict[str, float]:
        if self._model is None:
            return {}
        names = self._model.booster_.feature_name() if hasattr(self._model, "booster_") else []
        imps = self._model.feature_importances_.tolist() if hasattr(self._model, "feature_importances_") else []
        return dict(sorted(zip(names, imps), key=lambda x: x[1], reverse=True))
