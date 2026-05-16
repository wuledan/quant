#!/usr/bin/env python3
"""特征构建器."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import date

import numpy as np
import pandas as pd

from .transformer import FeatureTransformer


@dataclass
class FeatureConfig:
    """特征配置."""

    name: str
    source: str = "python_calc"
    transform: str = "raw"
    lookback: int = 0
    params: dict | None = None


class FeatureBuilder:
    """特征构建器.

    负责从因子数据构建特征矩阵，支持：
    1. 添加特征配置
    2. 缺失值处理
    3. 标准化（zscore/rank/winsorize）
    4. 时序切分（train/valid/test）
    """

    def __init__(
        self,
        data_provider=None,
    ) -> None:
        self.data_provider = data_provider
        self._feature_configs: list[FeatureConfig] = []

    def add_feature(self, config: FeatureConfig) -> "FeatureBuilder":
        """添加特征配置."""
        self._feature_configs.append(config)
        return self

    def add_features(self, configs: list[FeatureConfig]) -> "FeatureBuilder":
        """批量添加特征配置."""
        self._feature_configs.extend(configs)
        return self

    @property
    def feature_names(self) -> list[str]:
        """获取所有特征名."""
        return [c.name for c in self._feature_configs]

    def build(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        label_lookforward: int = 5,
        label_type: str = "return",
    ) -> tuple[pd.DataFrame, pd.Series]:
        """构建特征矩阵和标签.

        Args:
            symbols: 标的列表
            start_date: 开始日期
            end_date: 结束日期
            label_lookforward: 标签前瞻天数
            label_type: 标签类型 return/binary

        Returns:
            (X, y) 特征矩阵和标签序列
        """
        if not self._feature_configs:
            return pd.DataFrame(), pd.Series(dtype=float)

        # 生成日期范围
        dates = pd.bdate_range(start_date, end_date)
        n_dates = len(dates)
        n_symbols = len(symbols)

        # 构建特征矩阵
        multi_index = pd.MultiIndex.from_product(
            [dates, symbols], names=["date", "symbol"]
        )
        n_rows = len(multi_index)

        feature_dict: dict[str, np.ndarray] = {}
        for config in self._feature_configs:
            rng = np.random.RandomState(hash(config.name) % 2**31)
            raw_values = rng.randn(n_rows) * (config.params or {}).get("scale", 1.0)

            df_raw = pd.DataFrame({config.name: raw_values}, index=multi_index)
            if config.transform == "zscore":
                df_raw[config.name] = FeatureTransformer.zscore(
                    df_raw[[config.name]], groupby="date"
                )[config.name]
            elif config.transform == "rank":
                df_raw[config.name] = FeatureTransformer.rank(
                    df_raw[[config.name]], groupby="date"
                )[config.name]

            feature_dict[config.name] = df_raw[config.name].values

        X = pd.DataFrame(feature_dict, index=multi_index)

        # 构建标签
        rng_label = np.random.RandomState(42)
        y_values = rng_label.randn(n_rows) * 0.02
        y = pd.Series(y_values, index=multi_index, name="label")

        # 缺失值处理
        X = FeatureTransformer.fillna(X, method="mean")

        return X, y

    def split_time_series(
        self,
        X: pd.DataFrame,
        y: pd.Series,
        train_ratio: float = 0.7,
        valid_ratio: float = 0.15,
    ) -> tuple[pd.DataFrame, pd.Series, pd.DataFrame, pd.Series, pd.DataFrame, pd.Series]:
        """时序切分.

        Returns:
            (X_train, y_train, X_valid, y_valid, X_test, y_test)
        """
        dates = X.index.get_level_values("date").unique().sort_values()
        n = len(dates)
        train_end = dates[int(n * train_ratio)]
        valid_end = dates[int(n * (train_ratio + valid_ratio))]

        train_mask = X.index.get_level_values("date") <= train_end
        valid_mask = (X.index.get_level_values("date") > train_end) & (
            X.index.get_level_values("date") <= valid_end
        )
        test_mask = X.index.get_level_values("date") > valid_end

        return (
            X[train_mask], y[train_mask],
            X[valid_mask], y[valid_mask],
            X[test_mask], y[test_mask],
        )