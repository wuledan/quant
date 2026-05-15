"""全局配置 — Pydantic Settings.

支持环境变量覆盖，优先级：
环境变量 > .env文件 > 默认值

环境变量前缀: QI_ (如 QI_DATA_PROVIDER=akshare)
"""

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    """全局配置."""

    # ── 数据源 ──
    DATA_PROVIDER: str = "akshare"  # 默认数据源
    TUSHARE_TOKEN: str | None = None  # Tushare Pro Token
    WIND_USERNAME: str | None = None  # Wind账号
    WIND_PASSWORD: str | None = None

    # ── 存储 ──
    DATA_STORAGE_PATH: str = "./data"  # 数据存储路径
    CACHE_ENABLED: bool = True  # 是否启用本地缓存
    CACHE_PATH: str = "./cache"  # 缓存路径

    # ── C++引擎 ──
    CPP_ENGINE_MODE: str = "pybind11"  # "pybind11" | "arrow_ipc" | "grpc"
    CPP_ENGINE_CONFIG: dict = {}  # C++引擎配置

    # ── API ──
    API_HOST: str = "0.0.0.0"
    API_PORT: int = 8000
    API_WORKERS: int = 4
    JWT_SECRET: str = "change-me-in-production"
    JWT_EXPIRE_HOURS: int = 24

    # ── 飞书 ──
    FEISHU_APP_ID: str = ""
    FEISHU_APP_SECRET: str = ""
    FEISHU_VERIFICATION_TOKEN: str = ""
    FEISHU_WEBHOOK_URL: str = ""

    # ── 回测 ──
    BACKTEST_INITIAL_CASH: float = 1_000_000.0
    BACKTEST_COMMISSION_RATE: float = 0.00025
    BACKTEST_STAMP_TAX_RATE: float = 0.001

    # ── 日志 ──
    LOG_LEVEL: str = "INFO"
    LOG_PATH: str = "./logs"

    model_config = {"env_file": ".env", "env_prefix": "QI_"}


# 全局单例
settings = Settings()
