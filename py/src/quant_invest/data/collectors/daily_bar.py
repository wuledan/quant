"""日行情数据采集器"""

from __future__ import annotations

from datetime import date

import pandas as pd

from ..providers.base import AdjustMethod, DataProvider


class DailyBarCollector:
    """日行情数据采集器

    负责从数据源获取日线行情数据，执行数据质量校验后存储。
    """

    def __init__(self, provider: DataProvider) -> None:
        self._provider = provider

    def collect(
        self,
        symbol: str,
        start_date: date,
        end_date: date,
        adjust: AdjustMethod = AdjustMethod.FORWARD,
    ) -> pd.DataFrame:
        # TODO: 实现数据采集 + 质量校验
        raise NotImplementedError("DailyBarCollector.collect not implemented")
