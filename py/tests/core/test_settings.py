"""Settings核心配置测试."""

from __future__ import annotations

import os
from unittest.mock import patch

from quant_invest.core.settings import (
    APISettings,
    DatabaseSettings,
    FeishuSettings,
    LoggingSettings,
    RedisSettings,
    Settings,
    get_settings,
)


class TestSettings:
    """Settings默认值和分组配置测试."""

    def test_default_values(self):
        """测试默认值加载."""
        s = Settings()
        assert s.DATA_PROVIDER == "akshare"
        assert s.CACHE_ENABLED is True
        assert s.BACKTEST_INITIAL_CASH == 1_000_000.0
        assert s.CPP_ENGINE_MODE == "pybind11"

    def test_database_settings(self):
        """测试数据库配置分组."""
        db = DatabaseSettings()
        assert db.URL == "postgresql://localhost:5432/quant_invest"
        assert db.POOL_SIZE == 10
        assert db.MAX_OVERFLOW == 20

    def test_redis_settings(self):
        """测试Redis配置分组."""
        redis = RedisSettings()
        assert redis.PORT == 6379
        assert redis.DB == 0
        assert redis.PASSWORD is None

    def test_api_settings(self):
        """测试API配置分组."""
        api = APISettings()
        assert api.HOST == "0.0.0.0"
        assert api.PORT == 8000
        assert api.JWT_SECRET == "change-me-in-production"

    def test_feishu_settings(self):
        """测试飞书配置分组."""
        feishu = FeishuSettings()
        assert feishu.APP_ID == ""
        assert feishu.APP_SECRET == ""

    def test_logging_settings(self):
        """测试日志配置分组."""
        log = LoggingSettings()
        assert log.LEVEL == "INFO"
        assert log.FORMAT == "json"

    def test_settings_nesting(self):
        """测试嵌套分组."""
        s = Settings()
        assert s.DATABASE.URL == "postgresql://localhost:5432/quant_invest"
        assert s.API.PORT == 8000
        assert s.LOGGING.LEVEL == "INFO"

    def test_env_override(self):
        """测试环境变量覆盖."""
        with patch.dict(os.environ, {"QI_DATA_PROVIDER": "tushare"}):
            s = Settings()
            assert s.DATA_PROVIDER == "tushare"

    def test_env_override_nested(self):
        """测试嵌套分组的环境变量覆盖 — 使用正确的嵌套前缀."""
        # DatabaseSettings has env_prefix="QI_DB_", so the env var is QI_DB_URL
        # APISettings has env_prefix="QI_API_", so the env var is QI_API_PORT
        with patch.dict(
            os.environ,
            {
                "QI_DB_URL": "postgresql://custom:5432/testdb",
                "QI_API_PORT": "9000",
            },
        ):
            db = DatabaseSettings()
            api = APISettings()
            assert db.URL == "postgresql://custom:5432/testdb"
            assert api.PORT == 9000


class TestGetSettings:
    """get_settings单例测试."""

    def test_singleton(self):
        """测试单例模式."""
        s1 = get_settings()
        s2 = get_settings()
        assert s1 is s2

    def test_returns_settings(self):
        """测试返回类型."""
        s = get_settings()
        assert isinstance(s, Settings)
