"""配置模块.

Settings的规范入口在 core.settings，config.settings保留扁平配置供兼容。
"""

from quant_invest.config.logging_conf import setup_logging
from quant_invest.core.settings import Settings, get_settings

__all__ = ["Settings", "setup_logging", "get_settings"]
