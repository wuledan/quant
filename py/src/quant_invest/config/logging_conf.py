"""日志配置 — Loguru."""

import sys
from pathlib import Path

from loguru import logger

from quant_invest.config.settings import settings


def setup_logging(level: str | None = None, log_path: str | None = None) -> None:
    """配置日志系统.

    Args:
        level: 日志级别，默认使用 settings.LOG_LEVEL
        log_path: 日志路径，默认使用 settings.LOG_PATH
    """
    level = level or settings.LOG_LEVEL
    log_path = log_path or settings.LOG_PATH

    # 移除默认handler
    logger.remove()

    # 控制台输出
    logger.add(
        sys.stderr,
        level=level,
        format="<green>{time:YYYY-MM-DD HH:mm:ss}</green> | "
        "<level>{level: <8}</level> | "
        "<cyan>{name}</cyan>:<cyan>{function}</cyan>:<cyan>{line}</cyan> - "
        "<level>{message}</level>",
    )

    # 文件输出
    log_dir = Path(log_path)
    log_dir.mkdir(parents=True, exist_ok=True)
    logger.add(
        str(log_dir / "quant_invest_{time:YYYY-MM-DD}.log"),
        level=level,
        rotation="00:00",  # 每天轮换
        retention="30 days",  # 保留30天
        compression="gz",  # 压缩旧日志
        encoding="utf-8",
    )


__all__ = ["logger", "setup_logging"]
