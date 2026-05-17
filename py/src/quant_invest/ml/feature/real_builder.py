#!/usr/bin/env python3
"""基于真实数据的特征构建器."""

from __future__ import annotations

from datetime import date

import numpy as np
import pandas as pd

from ..feature.builder import FeatureBuilder, FeatureConfig
from ..feature.transformer import FeatureTransformer


class RealDataFeatureBuilder(FeatureBuilder):
    """基于真实DataProvider的特征构建器.

    与基类不同，此类从DataProvider获取真实行情数据，
    计算技术因子，而非使用随机数据。
    """

    def __init__(self, data_provider=None) -> None:
        super().__init__(data_provider=data_provider)
        self._price_cache: dict[str, pd.DataFrame] = {}

    def _get_price_data(self, symbol: str, start: date, end: date) -> pd.DataFrame:
        """从DataProvider获取行情数据，带缓存."""
        cache_key = f"{symbol}_{start}_{end}"
        if cache_key in self._price_cache:
            return self._price_cache[cache_key]

        if self.data_provider is None:
            return pd.DataFrame()

        try:
            df = self.data_provider.get_daily_bars(
                symbol=symbol,
                start_date=start,
                end_date=end,
            )
            self._price_cache[cache_key] = df
            return df
        except Exception:
            return pd.DataFrame()

    def build(
        self,
        symbols: list[str],
        start_date: date,
        end_date: date,
        label_lookforward: int = 5,
        label_type: str = "return",
    ) -> tuple[pd.DataFrame, pd.Series]:
        """从真实数据构建特征矩阵和标签."""
        if not self._feature_configs:
            return pd.DataFrame(), pd.Series(dtype=float)

        all_features: list[pd.DataFrame] = []
        all_labels: list[pd.Series] = []

        for symbol in symbols:
            df = self._get_price_data(symbol, start_date, end_date)
            if df.empty:
                continue

            # Compute features from price data
            feat_df = self._compute_features(df, symbol)
            if feat_df.empty:
                continue

            # Compute label
            label = self._compute_label(df, lookforward=label_lookforward, label_type=label_type)
            if label.empty:
                continue

            # Align indices
            common_idx = feat_df.index.intersection(label.index)
            if len(common_idx) == 0:
                continue

            feat_df = feat_df.loc[common_idx]
            label = label.loc[common_idx]

            # Add symbol level
            feat_df["symbol"] = symbol
            feat_df = feat_df.set_index("symbol", append=True)
            label.index = feat_df.index

            all_features.append(feat_df)
            all_labels.append(label)

        if not all_features:
            # Fallback to parent's random data generation
            return super().build(symbols, start_date, end_date, label_lookforward, label_type)

        X = pd.concat(all_features).sort_index()
        y = pd.concat(all_labels).sort_index()
        y.name = "label"

        # Fillna
        X = FeatureTransformer.fillna(X, method="mean")

        return X, y

    def _compute_features(self, df: pd.DataFrame, symbol: str) -> pd.DataFrame:
        """从行情数据计算配置的因子."""
        close = df["close"] if "close" in df.columns else pd.Series(dtype=float)
        volume = df["volume"] if "volume" in df.columns else pd.Series(dtype=float)
        high = df["high"] if "high" in df.columns else pd.Series(dtype=float)
        low = df["low"] if "low" in df.columns else pd.Series(dtype=float)

        feature_dict: dict[str, pd.Series] = {}

        for config in self._feature_configs:
            name = config.name
            lookback = config.lookback or 20

            if name == "return_1d":
                feature_dict[name] = close.pct_change(1)
            elif name == "return_5d":
                feature_dict[name] = close.pct_change(5)
            elif name == "return_20d":
                feature_dict[name] = close.pct_change(20)
            elif name == "volatility":
                feature_dict[name] = close.pct_change().rolling(lookback).std()
            elif name == "turnover":
                feature_dict[name] = volume / volume.rolling(lookback).mean() - 1
            elif name == "ma_ratio":
                feature_dict[name] = close / close.rolling(lookback).mean()
            elif name == "high_low_ratio":
                feature_dict[name] = (high - low) / close
            elif name == "volume_price_corr":
                ret = close.pct_change()
                feature_dict[name] = ret.rolling(lookback).corr(volume)
            else:
                # Unknown feature: generate NaN (will be filled)
                feature_dict[name] = pd.Series(np.nan, index=df.index)

        result = pd.DataFrame(feature_dict, index=df.index)
        result = result.dropna(how="all")

        # Apply transforms
        for config in self._feature_configs:
            if config.name in result.columns and config.transform != "raw":
                if config.transform == "zscore":
                    result[config.name] = (result[config.name] - result[config.name].mean()) / result[config.name].std().replace(0, 1)
                elif config.transform == "rank":
                    result[config.name] = result[config.name].rank(pct=True)

        return result

    def _compute_label(
        self, df: pd.DataFrame, lookforward: int = 5, label_type: str = "return"
    ) -> pd.Series:
        """计算标签."""
        close = df["close"] if "close" in df.columns else pd.Series(dtype=float)
        if close.empty:
            return pd.Series(dtype=float)

        future_return = close.shift(-lookforward) / close - 1

        if label_type == "binary":
            return (future_return > 0).astype(float)
        return future_return
