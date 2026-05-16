#!/usr/bin/env python3
"""特征变换管道."""

from __future__ import annotations

import pandas as pd


class FeatureTransformer:
    """特征变换管道."""

    @staticmethod
    def zscore(df: pd.DataFrame, groupby: str = "date") -> pd.DataFrame:
        """横截面标准化."""
        return df.groupby(groupby).transform(lambda x: (x - x.mean()) / x.std())

    @staticmethod
    def rank(df: pd.DataFrame, groupby: str = "date") -> pd.DataFrame:
        """横截面排名."""
        return df.groupby(groupby).rank(pct=True)

    @staticmethod
    def winsorize(df: pd.DataFrame, n_sigma: float = 3.0) -> pd.DataFrame:
        """去极值."""
        mean = df.mean()
        std = df.std()
        return df.clip(mean - n_sigma * std, mean + n_sigma * std, axis=1)

    @staticmethod
    def fillna(df: pd.DataFrame, method: str = "ffill") -> pd.DataFrame:
        """缺失值填充."""
        if method == "ffill":
            return df.ffill()
        elif method == "bfill":
            return df.bfill()
        else:
            return df.fillna(df.mean())
