"""核心配置 — Pydantic Settings.

支持环境变量覆盖，优先级：环境变量 > .env文件 > 默认值
环境变量前缀: QI_
"""

from __future__ import annotations

from pydantic_settings import BaseSettings, SettingsConfigDict


class DatabaseSettings(BaseSettings):
    """数据库配置分组."""

    URL: str = "postgresql://localhost:5432/quant_invest"
    POOL_SIZE: int = 10
    MAX_OVERFLOW: int = 20
    ECHO: bool = False

    model_config = SettingsConfigDict(env_prefix="QI_DB_")


class RedisSettings(BaseSettings):
    """Redis配置分组."""

    URL: str = "redis://localhost:6379/0"
    PORT: int = 6379
    DB: int = 0
    PASSWORD: str | None = None

    model_config = SettingsConfigDict(env_prefix="QI_REDIS_")


class APISettings(BaseSettings):
    """API服务配置分组."""

    HOST: str = "0.0.0.0"
    PORT: int = 8000
    WORKERS: int = 4
    CORS_ORIGINS: list[str] = ["*"]
    JWT_SECRET: str = "change-me-in-production"
    JWT_EXPIRE_HOURS: int = 24

    model_config = SettingsConfigDict(env_prefix="QI_API_")


class FeishuSettings(BaseSettings):
    """飞书机器人配置分组."""

    APP_ID: str = ""
    APP_SECRET: str = ""
    ENCRYPT_KEY: str = ""
    VERIFICATION_TOKEN: str = ""
    WEBHOOK_URL: str = ""

    model_config = SettingsConfigDict(env_prefix="QI_FEISHU_")


class LoggingSettings(BaseSettings):
    """日志配置分组."""

    LEVEL: str = "INFO"
    FORMAT: str = "json"
    FILE_PATH: str = "./logs/quant_invest.log"

    model_config = SettingsConfigDict(env_prefix="QI_LOG_")


class Settings(BaseSettings):
    """全局设置."""

    # 数据源
    DATA_PROVIDER: str = "akshare"
    TUSHARE_TOKEN: str | None = None
    WIND_USERNAME: str | None = None
    WIND_PASSWORD: str | None = None

    # 存储
    DATA_STORAGE_PATH: str = "./data"
    CACHE_ENABLED: bool = True
    CACHE_PATH: str = "./cache"

    # C++引擎
    CPP_ENGINE_MODE: str = "pybind11"
    CPP_ENGINE_CONFIG: dict = {}

    # 回测
    BACKTEST_INITIAL_CASH: float = 1_000_000.0
    BACKTEST_COMMISSION_RATE: float = 0.00025
    BACKTEST_STAMP_TAX_RATE: float = 0.001

    # 分组配置
    DATABASE: DatabaseSettings = DatabaseSettings()
    REDIS: RedisSettings = RedisSettings()
    API: APISettings = APISettings()
    FEISHU: FeishuSettings = FeishuSettings()
    LOGGING: LoggingSettings = LoggingSettings()

    model_config = SettingsConfigDict(env_file=".env", env_prefix="QI_", extra="ignore")


_settings: Settings | None = None


def get_settings() -> Settings:
    """获取全局Settings单例（lazy initialization）."""
    global _settings
    if _settings is None:
        _settings = Settings()
    return _settings
