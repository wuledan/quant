"""空DataFrame边界测试."""

from __future__ import annotations

from datetime import date

import pandas as pd

from quant_invest.data.quality.anomaly_detector import AnomalyDetector
from quant_invest.data.quality.repair import DataRepair
from quant_invest.data.quality.validator import DataValidator, QualityLevel


class TestEmptyDataFrame:
    """空DataFrame边界条件测试."""

    def test_validator_empty_df(self):
        """验证空DataFrame."""
        validator = DataValidator()
        report = validator.validate(
            pd.DataFrame(),
            symbol="TEST",
            start_date=date(2024, 1, 1),
            end_date=date(2024, 1, 31),
        )
        assert report.level == QualityLevel.ERROR

    def test_anomaly_detector_empty_df(self):
        """异常检测空DataFrame."""
        detector = AnomalyDetector()
        result = detector.detect(pd.DataFrame())
        assert len(result) == 0

    def test_data_repair_fill_forward_empty(self):
        """空DataFrame前值填充."""
        repair = DataRepair()
        result = repair.fill_forward(pd.DataFrame())
        assert result.empty
