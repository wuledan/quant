"""数据源适配器.

统一导出自定义数据源适配器和工厂函数。
"""

from quant_invest.data.providers.akshare_provider import AkshareProvider
from quant_invest.data.providers.base import AdjustMethod, DataFreq, DataProvider


class DataProviderFactory:
    """数据源工厂.

    根据配置名称创建对应的数据源实例。
    """

    _providers: dict[str, type[DataProvider]] = {
        "akshare": AkshareProvider,
    }

    @classmethod
    def register(cls, name: str, provider_cls: type[DataProvider]) -> None:
        """注册自定义数据源."""
        cls._providers[name] = provider_cls

    @classmethod
    def create(
        cls,
        name: str = "akshare",
        config: dict | None = None,
    ) -> DataProvider:
        """创建数据源实例.

        Args:
            name: 数据源名称 (akshare / tushare / wind)
            config: 配置字典

        Returns:
            DataProvider 实例

        Raises:
            ValueError: 未知的数据源名称
        """
        provider_cls = cls._providers.get(name)
        if provider_cls is None:
            raise ValueError(f"Unknown provider: {name}. Available: {list(cls._providers.keys())}")
        return provider_cls(config=config or {})

    @classmethod
    def available_providers(cls) -> list[str]:
        """列出所有已注册的数据源."""
        return list(cls._providers.keys())


__all__ = [
    "DataProvider",
    "DataFreq",
    "AdjustMethod",
    "AkshareProvider",
    "DataProviderFactory",
]
