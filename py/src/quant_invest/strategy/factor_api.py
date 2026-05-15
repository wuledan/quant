"""因子API - Python与C++因子引擎的桥梁

交互方式：
1. pybind11 直接调用C++因子计算函数（推荐，低延迟）
2. Arrow IPC 共享内存通信（大批量数据传输）
3. HTTP/gRPC 远程调用（分布式场景）
"""

from __future__ import annotations

from datetime import date

import pandas as pd


class FactorAPI:
    """因子API"""

    def __init__(
        self,
        engine: str = "cpp_binding",
        config: dict | None = None,
    ) -> None:
        self._engine_type = engine
        self._config = config or {}
        self._engine: object | None = None
        self._initialize_engine()

    def calculate(
        self,
        factor_name: str,
        symbols: list[str],
        date: date,
        **kwargs: object,
    ) -> pd.Series:
        """计算单个因子

        Args:
            factor_name: 因子名（需在C++引擎中注册）
            symbols: 标的列表
            date: 计算日期
            **kwargs: 因子特有参数

        Returns:
            pd.Series, index=symbol, values=factor_value
        """
        # TODO: 实现因子计算
        raise NotImplementedError("FactorAPI.calculate not implemented")

    def calculate_batch(
        self,
        factor_names: list[str],
        symbols: list[str],
        start_date: date,
        end_date: date,
    ) -> pd.DataFrame:
        """批量计算多个因子"""
        # TODO: 实现
        raise NotImplementedError("FactorAPI.calculate_batch not implemented")

    def list_factors(self) -> list[dict]:
        """列出所有可用因子及其说明"""
        # TODO: 实现
        raise NotImplementedError("FactorAPI.list_factors not implemented")

    def _initialize_engine(self) -> None:
        """初始化C++因子引擎连接"""
        if self._engine_type == "cpp_binding":
            # TODO: 导入C++绑定
            pass
        elif self._engine_type == "arrow_ipc":
            # TODO: Arrow IPC 初始化
            pass
        elif self._engine_type == "grpc":
            # TODO: gRPC 客户端初始化
            pass
