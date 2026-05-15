"""数据修复工具."""

from __future__ import annotations

import pandas as pd


class DataRepair:
    """数据修复器."""

    @staticmethod
    def fill_forward(
        df: pd.DataFrame,
        columns: list[str] | None = None,
        limit: int | None = None,
    ) -> pd.DataFrame:
        """前值填充."""
        result = df.copy()
        cols = columns or result.columns.tolist()
        result[cols] = result[cols].ffill(limit=limit)
        return result

    @staticmethod
    def fill_backward(
        df: pd.DataFrame,
        columns: list[str] | None = None,
        limit: int | None = None,
    ) -> pd.DataFrame:
        """后值填充."""
        result = df.copy()
        cols = columns or result.columns.tolist()
        result[cols] = result[cols].bfill(limit=limit)
        return result

    @staticmethod
    def interpolate(
        df: pd.DataFrame,
        columns: list[str] | None = None,
        method: str = "linear",
    ) -> pd.DataFrame:
        """插值填充."""
        result = df.copy()
        cols = columns or result.select_dtypes(include=["float64", "int64"]).columns.tolist()
        result[cols] = result[cols].interpolate(method=method)
        return result
