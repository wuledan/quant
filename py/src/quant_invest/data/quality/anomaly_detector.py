"""异常数据检测与修复."""

from __future__ import annotations

import pandas as pd


class AnomalyDetector:
    """异常数据检测与修复."""

    @staticmethod
    def detect(
        df: pd.DataFrame,
        columns: list[str] | None = None,
        method: str = "zscore",
        threshold: float = 3.0,
    ) -> pd.Series:
        """检测异常行索引.

        Args:
            df: 输入DataFrame
            columns: 待检测列，None表示所有数值列
            method: 检测方法 ("zscore" / "iqr")
            threshold: 异常阈值

        Returns:
            boolean Series，True表示异常行
        """
        if df.empty:
            return pd.Series(dtype=bool)

        cols = columns or df.select_dtypes(include=["float64", "int64"]).columns.tolist()
        if not cols:
            return pd.Series(False, index=df.index)

        if method == "zscore":
            return AnomalyDetector._detect_zscore(df[cols], threshold)
        elif method == "iqr":
            return AnomalyDetector._detect_iqr(df[cols])
        else:
            raise ValueError(f"Unknown detection method: {method}")

    @staticmethod
    def _detect_zscore(df: pd.DataFrame, threshold: float = 3.0) -> pd.Series:
        """基于Z-score检测."""
        mean = df.mean()
        std = df.std()
        zscores = (df - mean).abs() / std.replace(0, float("inf"))
        return zscores.max(axis=1) > threshold

    @staticmethod
    def _detect_iqr(df: pd.DataFrame) -> pd.Series:
        """基于IQR检测."""
        q1 = df.quantile(0.25)
        q3 = df.quantile(0.75)
        iqr = q3 - q1
        lower = q1 - 1.5 * iqr
        upper = q3 + 1.5 * iqr
        return ((df < lower) | (df > upper)).any(axis=1)
