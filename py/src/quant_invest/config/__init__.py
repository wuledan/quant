"""配置模块.

Settings的规范入口在 core.settings，config.settings保留扁平配置供兼容。
"""

from quant_invest.config.logging_conf import setup_logging
from quant_invest.config.settings import Settings

# 推荐使用 core.settings 中的分组配置
from quant_invest.core.settings import Settings as CoreSettings, get_settings

__all__ = ["Settings", "setup_logging", "CoreSettings", "get_settings"]
