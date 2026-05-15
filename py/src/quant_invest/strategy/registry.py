"""策略注册中心"""

from __future__ import annotations

from .base import StrategyBase


class StrategyRegistry:
    """策略注册中心

    提供策略的统一注册、发现和实例化：
    - 通过装饰器自动注册策略
    - 支持按名称查找策略类
    - 支持策略版本管理
    """

    _registry: dict[str, type[StrategyBase]] = {}

    @classmethod
    def register(cls, name: str):
        """策略注册装饰器"""

        def decorator(strategy_cls: type[StrategyBase]) -> type[StrategyBase]:
            cls._registry[name] = strategy_cls
            strategy_cls._registry_name = name  # type: ignore[attr-defined]
            return strategy_cls

        return decorator

    @classmethod
    def get(cls, name: str) -> type[StrategyBase]:
        """按名称查找策略"""
        if name not in cls._registry:
            raise KeyError(f"Strategy '{name}' not registered")
        return cls._registry[name]

    @classmethod
    def list_strategies(cls) -> list[str]:
        """列出所有已注册策略"""
        return list(cls._registry.keys())
