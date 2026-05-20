#!/usr/bin/env python3
"""P0 集成测试 — 验证 StorageEngine + DataSchedulerService + query_kline 全链路.

测试场景:
1. StorageEngineAdapter 初始化 + Parquet 加载
2. DataSchedulerService C++ 模式数据查询
3. Python/C++ 模式数据一致性对比
4. StorageEngine 单行写入（模拟实时更新）
"""

import sys
from datetime import date, timedelta
from pathlib import Path

import pandas as pd
import pytest

# 确保项目路径在 sys.path 中
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "py" / "src"))


class TestP0Integration:
    """P0 集成测试."""

    @pytest.fixture
    def data_dir(self) -> Path:
        """数据目录."""
        return PROJECT_ROOT / "py" / "data" / "daily"

    def test_storage_adapter_init_and_load(self) -> None:
        """测试 StorageEngineAdapter 初始化和 Parquet 加载."""
        from quant_invest.storage.adapter import StorageEngineAdapter

        data_dir = str((PROJECT_ROOT / "py" / "data" / "daily").resolve())
        adapter = StorageEngineAdapter(data_dir=data_dir)
        results = adapter.load_all_cached()

        assert len(results) > 0, "应该至少加载一个标的"
        for symbol, count in results.items():
            assert count > 0, f"{symbol} 应有数据行"

        adapter.close()

    def test_storage_adapter_query(self) -> None:
        """测试 StorageEngineAdapter 查询功能."""
        from quant_invest.storage.adapter import StorageEngineAdapter

        data_dir = str((PROJECT_ROOT / "py" / "data" / "daily").resolve())
        adapter = StorageEngineAdapter(data_dir=data_dir)
        adapter.load_all_cached()

        # 测试 query_kline
        result = adapter.query_kline("000001.SH", field="close")
        assert len(result) > 0, "应该有查询结果"
        assert "timestamp" in result[0], "结果应包含 timestamp"
        assert "value" in result[0], "结果应包含 value"

        # 测试 query_kline_as_df
        df = adapter.query_kline_as_df("000001.SH")
        assert not df.empty, "DataFrame 不应为空"
        assert "close" in df.columns, "DataFrame 应有 close 列"

        adapter.close()

    def test_scheduler_cpp_mode(self) -> None:
        """测试 DataSchedulerService C++ 模式."""
        from quant_invest.data.scheduler import DataSchedulerService

        svc = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
            use_cpp_storage=True,
        )

        assert svc.use_cpp_storage, "C++ StorageEngine 应可用"

        # 查询数据
        df = svc.get_daily_data("000001.SH")
        assert not df.empty, "应返回数据"
        assert "close" in df.columns, "应包含 close 列"

    def test_scheduler_python_mode(self) -> None:
        """测试 DataSchedulerService Python 模式."""
        from quant_invest.data.scheduler import DataSchedulerService

        svc = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
        )

        assert not svc.use_cpp_storage, "Python 模式不应使用 C++ Storage"

        df = svc.get_daily_data("000001.SH")
        assert not df.empty, "应返回数据"

    def test_data_consistency(self) -> None:
        """测试 Python/C++ 模式数据一致性."""
        from quant_invest.data.scheduler import DataSchedulerService

        svc_py = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
        )
        svc_cpp = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
            use_cpp_storage=True,
        )

        symbols = ["000001.SH", "600519.SH", "000300.SH"]
        for symbol in symbols:
            df_py = svc_py.get_daily_data(symbol)
            df_cpp = svc_cpp.get_daily_data(symbol)

            if df_py.empty or df_cpp.empty:
                continue

            # 行数应一致
            assert len(df_py) == len(df_cpp), f"{symbol}: 行数应一致"

            # 最新收盘价应一致
            py_close = df_py["close"].iloc[-1]
            cpp_close = df_cpp["close"].iloc[-1]
            assert abs(py_close - cpp_close) < 0.01, f"{symbol}: 收盘价应一致"

    def test_storage_adapter_realtime_write(self) -> None:
        """测试 StorageEngineAdapter 实时单行写入."""
        from quant_invest.storage.adapter import StorageEngineAdapter

        data_dir = str((PROJECT_ROOT / "py" / "data" / "daily").resolve())
        adapter = StorageEngineAdapter(data_dir=data_dir)
        adapter.load_all_cached()

        # 模拟实时更新
        adapter.store_kline("000001.SH", {
            "timestamp": 0,  # 使用当前时间
            "open": 3400.0,
            "high": 3410.0,
            "low": 3390.0,
            "close": 3405.0,
            "volume": 1000000,
            "amount": 3405000000,
            "vwap": 3402.5,
        })

        # 查询验证
        df = adapter.query_kline_as_df("000001.SH")
        assert not df.empty, "写入后应有数据"

        adapter.close()

    def test_scheduler_status(self) -> None:
        """测试调度器状态查询."""
        from quant_invest.data.scheduler import DataSchedulerService

        svc = DataSchedulerService(
            storage_path=str(PROJECT_ROOT / "py" / "data"),
            use_cpp_storage=True,
        )

        status = svc.get_status()
        assert "running" in status
        assert "cpp_storage" in status
        assert status["cpp_storage"] is True
        assert "daily_cache_count" in status