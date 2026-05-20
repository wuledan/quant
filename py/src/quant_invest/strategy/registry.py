#!/usr/bin/env python3
"""策略注册表 — 管理策略类的注册、查找、列举、热重载.

支持两种策略类型共存:
1. 传统 on_bar 策略 (StrategyBase 子类)
2. DSL 声明式策略 (Strategy 子类, 带 Factor/SignalExpr 声明)

注册表为每个策略记录:
- 策略类型 (on_bar / dsl)
- 策略类引用
- DSL 策略的 Factor/SignalExpr 声明 (用于构建 DAG)
- 策略来源模块 (用于热重载)
"""

from __future__ import annotations

import importlib
import logging
import sys
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any

logger = logging.getLogger("quant_invest.strategy.registry")


class StrategyKind(Enum):
    """策略类型."""

    ON_BAR = auto()  # 传统 on_bar 策略
    DSL = auto()     # 声明式 DSL 策略


@dataclass
class StrategyEntry:
    """注册表中的策略条目."""

    name: str
    cls: type
    kind: StrategyKind
    module_name: str = ""
    module_file: str = ""
    # DSL 策略专用
    factor_decls: dict[str, Any] = field(default_factory=dict)
    signal_decls: dict[str, Any] = field(default_factory=dict)

    @property
    def is_dsl(self) -> bool:
        return self.kind == StrategyKind.DSL


class _StrategyRegistry:
    """策略注册表单例.

    功能:
    - register(): 装饰器注册策略类
    - get(): 获取策略类
    - get_entry(): 获取策略条目（含类型、DAG 信息）
    - list_strategies(): 列出策略名
    - list_by_kind(): 按类型筛选策略
    - reload(): 热重载指定策略模块
    - reload_all(): 热重载所有策略模块
    """

    def __init__(self) -> None:
        self._registry: dict[str, StrategyEntry] = {}

    def register(self, name: str):
        """装饰器: 注册策略类.

        自动检测策略类型:
        - 带 _dsl_strategy 标记 → DSL 策略
        - 否则 → 传统 on_bar 策略

        用法::

            @StrategyRegistry.register("my_strategy")
            class MyStrategy(StrategyBase):
                ...
        """
        def decorator(cls: type) -> type:
            if name in self._registry:
                logger.warning("策略 '%s' 已存在，将被覆盖", name)

            # 检测策略类型
            is_dsl = getattr(cls, "_dsl_strategy", False)
            kind = StrategyKind.DSL if is_dsl else StrategyKind.ON_BAR

            # 收集 DSL 声明
            factor_decls = getattr(cls, "_factor_decls", {})
            signal_decls = getattr(cls, "_signal_decls", {})

            # 记录来源模块
            module_name = cls.__module__
            module = sys.modules.get(module_name)
            module_file = getattr(module, "__file__", "") if module else ""

            entry = StrategyEntry(
                name=name,
                cls=cls,
                kind=kind,
                module_name=module_name,
                module_file=module_file,
                factor_decls=factor_decls,
                signal_decls=signal_decls,
            )
            self._registry[name] = entry
            logger.debug(
                "策略注册: %s → %s (%s)",
                name,
                cls.__name__,
                kind.name,
            )
            return cls

        return decorator

    def get(self, name: str) -> type:
        """获取策略类，未找到时抛出 KeyError."""
        self._ensure_loaded()
        if name not in self._registry:
            raise KeyError(
                f"策略 '{name}' 未注册，可用: {list(self._registry.keys())}"
            )
        return self._registry[name].cls

    def get_entry(self, name: str) -> StrategyEntry:
        """获取策略条目（含类型、DAG 信息），未找到时抛出 KeyError."""
        self._ensure_loaded()
        if name not in self._registry:
            raise KeyError(
                f"策略 '{name}' 未注册，可用: {list(self._registry.keys())}"
            )
        return self._registry[name]

    def list_strategies(self) -> list[str]:
        """列出所有已注册策略名."""
        self._ensure_loaded()
        return list(self._registry.keys())

    def list_by_kind(self, kind: StrategyKind) -> list[str]:
        """按类型筛选策略名."""
        self._ensure_loaded()
        return [name for name, entry in self._registry.items() if entry.kind == kind]

    def list_dsl_strategies(self) -> list[str]:
        """列出所有 DSL 策略名."""
        return self.list_by_kind(StrategyKind.DSL)

    def list_on_bar_strategies(self) -> list[str]:
        """列出所有传统 on_bar 策略名."""
        return self.list_by_kind(StrategyKind.ON_BAR)

    def has(self, name: str) -> bool:
        """检查策略是否已注册."""
        self._ensure_loaded()
        return name in self._registry

    def get_dag_info(self, name: str) -> dict[str, Any]:
        """获取 DSL 策略的 DAG 信息.

        Returns:
            {
                "factors": {name: Factor},
                "signals": {name: SignalExpr},
                "dag_nodes": [DAGNode, ...],  # 拓扑序
            }
        """
        entry = self.get_entry(name)
        if not entry.is_dsl:
            raise ValueError(f"策略 '{name}' 不是 DSL 策略，无 DAG 信息")

        # 实例化策略以获取 DAG 节点
        instance = entry.cls()
        return {
            "factors": entry.factor_decls,
            "signals": entry.signal_decls,
            "dag_nodes": instance.get_dag_nodes(),
        }

    def reload(self, name: str) -> bool:
        """热重载指定策略模块.

        重新导入策略所在的 Python 模块，触发 @register 装饰器
        重新注册。如果模块不存在或重载失败，返回 False。

        Args:
            name: 策略名

        Returns:
            是否重载成功
        """
        self._ensure_loaded()
        if name not in self._registry:
            logger.warning("策略 '%s' 未注册，无法重载", name)
            return False

        entry = self._registry[name]
        module_name = entry.module_name
        if not module_name:
            logger.warning("策略 '%s' 无模块信息，无法重载", name)
            return False

        try:
            module = sys.modules.get(module_name)
            if module is None:
                logger.warning("模块 '%s' 未加载，无法重载", module_name)
                return False

            # 先移除旧注册，避免 "已存在" 警告
            del self._registry[name]

            # 重新导入模块
            importlib.reload(module)
            logger.info("策略 '%s' 重载成功 (from %s)", name, module_name)
            return True
        except Exception as e:
            logger.error("策略 '%s' 重载失败: %s", name, e)
            return False

    def reload_all(self) -> dict[str, bool]:
        """热重载所有策略模块.

        Returns:
            {策略名: 是否重载成功}
        """
        self._ensure_loaded()
        results: dict[str, bool] = {}
        # 收集所有需要重载的模块（去重）
        modules_to_reload: dict[str, list[str]] = {}
        for name, entry in self._registry.items():
            mod = entry.module_name
            if mod and mod not in modules_to_reload:
                modules_to_reload[mod] = []
            if mod:
                modules_to_reload[mod].append(name)

        # 逐模块重载
        for mod_name, strategy_names in modules_to_reload.items():
            module = sys.modules.get(mod_name)
            if module is None:
                for sn in strategy_names:
                    results[sn] = False
                continue

            try:
                # 移除旧注册
                for sn in strategy_names:
                    self._registry.pop(sn, None)

                importlib.reload(module)
                for sn in strategy_names:
                    results[sn] = self.has(sn)
                logger.info("模块 '%s' 重载成功", mod_name)
            except Exception as e:
                logger.error("模块 '%s' 重载失败: %s", mod_name, e)
                for sn in strategy_names:
                    results[sn] = False

        return results

    def unregister(self, name: str) -> bool:
        """取消注册策略.

        Args:
            name: 策略名

        Returns:
            是否成功取消
        """
        if name in self._registry:
            del self._registry[name]
            logger.debug("策略 '%s' 已取消注册", name)
            return True
        return False

    def _ensure_loaded(self) -> None:
        """确保策略模块已被导入（触发 @register 装饰器）."""
        if self._registry:
            return
        # 自动发现并导入策略模块
        self._auto_discover()

    def _auto_discover(self) -> None:
        """自动发现并导入策略模块."""
        modules_to_import = [
            ".ma_cross",           # 传统 on_bar 策略
            ".examples.ma_cross_dsl",  # DSL 策略示例
        ]
        for mod_path in modules_to_import:
            try:
                importlib.import_module(mod_path, package="quant_invest.strategy")
                logger.debug("自动导入策略模块: %s", mod_path)
            except ImportError as e:
                logger.debug("策略模块 %s 导入跳过: %s", mod_path, e)

    def summary(self) -> dict[str, Any]:
        """返回注册表摘要信息."""
        self._ensure_loaded()
        return {
            "total": len(self._registry),
            "dsl_count": len(self.list_dsl_strategies()),
            "on_bar_count": len(self.list_on_bar_strategies()),
            "strategies": {
                name: {
                    "kind": entry.kind.name,
                    "class": entry.cls.__name__,
                    "module": entry.module_name,
                    "factors": list(entry.factor_decls.keys()),
                    "signals": list(entry.signal_decls.keys()),
                }
                for name, entry in self._registry.items()
            },
        }


# 全局单例
StrategyRegistry = _StrategyRegistry()
