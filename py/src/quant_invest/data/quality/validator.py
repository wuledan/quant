"""数据质量校验."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import date
from enum import Enum

import pandas as pd


class QualityLevel(str, Enum):
    """质量等级."""

    PASS = "pass"
    WARNING = "warning"
    ERROR = "error"


@dataclass
class QualityReport:
    """数据质量报告."""

    level: QualityLevel
    checks: list[dict] = field(default_factory=list)
    missing_dates: list[date] = field(default_factory=list)
    anomalous_rows: list[int] = field(default_factory=list)
    summary: str = ""


class DataValidator:
    """数据质量校验器."""

    def __init__(self, config: dict | None = None) -> None:
        self.config = config or {}

    def validate(
        self,
        df: pd.DataFrame,
        symbol: str,
        start_date: date,
        end_date: date,
    ) -> QualityReport:
        """综合校验数据质量."""
        checks: list[dict] = []
        missing_dates: list[date] = []
        anomalous_rows: list[int] = []

        # 1. OHLC一致性
        if not df.empty:
            consistency_checks = self._check_consistency(df)
            checks.extend(consistency_checks)

        # 2. 完整性
        completeness_checks, missing = self._check_completeness(df, start_date, end_date)
        checks.extend(completeness_checks)
        missing_dates.extend(missing)

        # 3. 缺失值
        nan_checks = self._check_nan(df)
        checks.extend(nan_checks)

        # 4. 异常值
        outlier_checks, anomalies = self._check_outliers(df)
        checks.extend(outlier_checks)
        anomalous_rows.extend(anomalies)

        # 综合判定
        errors = [c for c in checks if c.get("status") == QualityLevel.ERROR]
        warnings = [c for c in checks if c.get("status") == QualityLevel.WARNING]

        if errors:
            level = QualityLevel.ERROR
        elif warnings:
            level = QualityLevel.WARNING
        else:
            level = QualityLevel.PASS

        return QualityReport(
            level=level,
            checks=checks,
            missing_dates=missing_dates,
            anomalous_rows=anomalous_rows,
            summary=f"{symbol}: {len(errors)} errors, {len(warnings)} warnings",
        )

    def _check_consistency(self, df: pd.DataFrame) -> list[dict]:
        """OHLC关系一致性校验."""
        issues: list[dict] = []
        # Invariant checks: (col_a, col_b, relation, check_func)
        # check_func returns True if the relation holds for all rows
        checks = [
            ("high", "low", "high >= low", lambda d: d["high"] >= d["low"]),
            ("high", "open", "high >= open", lambda d: d["high"] >= d["open"]),
            ("high", "close", "high >= close", lambda d: d["high"] >= d["close"]),
            ("low", "open", "low <= open", lambda d: d["low"] <= d["open"]),
            ("low", "close", "low <= close", lambda d: d["low"] <= d["close"]),
        ]
        for col_a, col_b, label, check_fn in checks:
            if all(c in df.columns for c in [col_a, col_b]):
                if not check_fn(df).all():
                    issues.append(
                        {
                            "name": f"consistency_{label}",
                            "status": QualityLevel.ERROR,
                            "detail": f"{label} 关系不成立",
                        }
                    )
        return issues

    def _check_completeness(
        self, df: pd.DataFrame, start_date: date, end_date: date
    ) -> tuple[list[dict], list[date]]:
        """数据完整性校验."""
        if df.empty:
            return (
                [{"name": "completeness", "status": QualityLevel.ERROR, "detail": "空DataFrame"}],
                [],
            )
        missing: list[date] = []
        # TODO: 基于交易日历检测缺失
        return (
            [{"name": "completeness", "status": QualityLevel.PASS, "detail": f"{len(df)} rows"}],
            missing,
        )

    def _check_nan(self, df: pd.DataFrame) -> list[dict]:
        """检查缺失值占比."""
        nan_ratio = df.isna().mean()
        if len(nan_ratio) == 0:
            return []
        max_nan = nan_ratio.max()
        threshold_warn = self.config.get("nan_warn_threshold", 0.05)
        threshold_err = self.config.get("nan_err_threshold", 0.2)

        if max_nan > threshold_err:
            status = QualityLevel.ERROR
        elif max_nan > threshold_warn:
            status = QualityLevel.WARNING
        else:
            status = QualityLevel.PASS

        return [{"name": "nan_check", "status": status, "detail": f"max NaN ratio: {max_nan:.2%}"}]

    def _check_outliers(self, df: pd.DataFrame) -> tuple[list[dict], list[int]]:
        """基于Z-score检测异常值."""
        numeric_cols = df.select_dtypes(include=["float64", "int64"]).columns
        anomalies: set[int] = set()
        for col in numeric_cols:
            mean = df[col].mean()
            std = df[col].std()
            if std == 0:
                continue
            zscores = (df[col] - mean).abs() / std
            anomalies.update(df[zscores > 3].index.tolist())

        threshold = self.config.get("outlier_threshold", 0.01)
        anomaly_ratio = len(anomalies) / max(len(df), 1)
        if anomaly_ratio > threshold:
            status = QualityLevel.WARNING
        else:
            status = QualityLevel.PASS

        return (
            [
                {
                    "name": "outlier",
                    "status": status,
                    "detail": f"{len(anomalies)} outliers ({anomaly_ratio:.2%})",
                }
            ],
            sorted(anomalies),
        )
