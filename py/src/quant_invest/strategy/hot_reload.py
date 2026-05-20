#!/usr/bin/env python3
"""策略热重载管理器 — 协调文件监视与策略编译.

HotReloadManager 是策略热重载的核心协调者:
1. 持有 StrategyWatcher 实例，监听策略文件变更
2. 持有 StrategyCompiler 实例，编译 DSL 策略
3. 维护编译缓存 dict[str, CompiledStrategy]
4. 当策略重载成功时，重新编译并更新缓存
5. 当策略重载失败时，保留旧版本编译结果（降级保护）
6. 提供 get_compiled(name) 供回测/交易引擎使用

用法::

    from quant_invest.strategy.hot_reload import HotReloadManager

    manager = HotReloadManager()
    manager.start()  # 启动文件监视

    # 获取编译后的策略
    compiled = manager.get_compiled("ma_cross_dsl")

    # 手动触发重载
    success = manager.reload_strategy("ma_cross_dsl")

    # 停止
    manager.stop()
"""

from __future__ import annotations

import logging
import threading
from dataclasses import dataclass, field
from typing import Any

from .compiler import CompiledStrategy, StrategyCompiler
from .registry import StrategyEntry, StrategyKind, StrategyRegistry
from .watcher import StrategyWatcher, get_watcher

logger = logging.getLogger("quant_invest.strategy.hot_reload")


@dataclass
class ReloadResult:
    """重载操作结果."""

    strategy_name: str
    success: bool
    compiled: bool  # 是否成功编译
    error_message: str = ""
    previous_version_kept: bool = False  # 是否保留了旧版本


class HotReloadManager:
    """策略热重载管理器 — 协调文件监视与策略编译.

    职责:
    - 启动/停止 StrategyWatcher
    - 监听 watcher 的成功/失败回调
    - 成功时重新编译策略并更新缓存
    - 失败时保留旧版本编译结果
    - 提供编译后策略的查询接口

    线程安全:
    - _cache 使用 RLock 保护（编译可能耗时，但读多写少）
    - watcher 回调在 watcher 线程中执行，需注意线程安全
    """

    def __init__(
        self,
        watcher: StrategyWatcher | None = None,
        compiler: StrategyCompiler | None = None,
        auto_start_watcher: bool = True,
    ) -> None:
        """初始化热重载管理器.

        Args:
            watcher: 文件监视器实例，默认使用全局单例
            compiler: 策略编译器实例，默认创建新实例
            auto_start_watcher: 是否在 start() 时自动启动 watcher
        """
        self._watcher = watcher or get_watcher()
        self._compiler = compiler or StrategyCompiler()
        self._auto_start_watcher = auto_start_watcher

        # 编译缓存: strategy_name → CompiledStrategy
        self._cache: dict[str, CompiledStrategy] = {}
        self._lock = threading.RLock()

        # 重载统计
        self._stats = _ReloadStats()

        # 注册 watcher 回调
        self._watcher.on_reload_success(self._on_reload_success)
        self._watcher.on_reload_failure(self._on_reload_failure)

        # 是否已启动
        self._started = False

    @property
    def watcher(self) -> StrategyWatcher:
        """获取关联的文件监视器."""
        return self._watcher

    @property
    def compiler(self) -> StrategyCompiler:
        """获取关联的策略编译器."""
        return self._compiler

    @property
    def is_started(self) -> bool:
        """管理器是否已启动."""
        return self._started

    def start(self) -> None:
        """启动热重载管理器.

        1. 预编译所有已注册的 DSL 策略
        2. 启动文件监视器
        """
        if self._started:
            logger.warning("HotReloadManager 已启动，忽略重复 start()")
            return

        # 预编译所有 DSL 策略
        self._precompile_all()

        # 启动 watcher
        if self._auto_start_watcher:
            self._watcher.start()

        self._started = True
        logger.info(
            "HotReloadManager 已启动 (缓存 %d 个策略, watcher %s)",
            len(self._cache),
            "运行中" if self._watcher.is_running else "未启动",
        )

    def stop(self) -> None:
        """停止热重载管理器."""
        if not self._started:
            return

        if self._auto_start_watcher:
            self._watcher.stop()

        self._started = False
        logger.info("HotReloadManager 已停止")

    def get_compiled(self, name: str) -> CompiledStrategy | None:
        """获取编译后的策略.

        如果缓存中没有，尝试编译并缓存。
        如果策略不存在或编译失败，返回 None。

        Args:
            name: 策略名

        Returns:
            CompiledStrategy 或 None
        """
        with self._lock:
            if name in self._cache:
                return self._cache[name]

        # 缓存未命中，尝试编译
        return self._compile_and_cache(name)

    def reload_strategy(self, name: str) -> ReloadResult:
        """手动触发策略重载 + 重新编译.

        流程:
        1. 调用 StrategyRegistry.reload(name) 重新导入模块
        2. 如果重载成功，重新编译策略
        3. 如果编译成功，更新缓存
        4. 如果编译失败，保留旧版本

        Args:
            name: 策略名

        Returns:
            ReloadResult
        """
        # Step 1: 触发 watcher 的重载（会调用 StrategyRegistry.reload）
        success = self._watcher.trigger_reload(name)

        if not success:
            return ReloadResult(
                strategy_name=name,
                success=False,
                compiled=False,
                error_message="StrategyRegistry.reload() failed",
            )

        # Step 2: 重新编译
        return self._recompile_strategy(name)

    def reload_all(self) -> dict[str, ReloadResult]:
        """重载所有策略.

        Returns:
            {策略名: ReloadResult}
        """
        results: dict[str, ReloadResult] = {}

        # 先重载所有注册的策略模块
        reload_results = StrategyRegistry.reload_all()

        for name, registry_success in reload_results.items():
            if registry_success:
                results[name] = self._recompile_strategy(name)
            else:
                results[name] = ReloadResult(
                    strategy_name=name,
                    success=False,
                    compiled=False,
                    error_message="Registry reload failed",
                )

        return results

    def get_status(self) -> dict[str, Any]:
        """获取热重载管理器状态.

        Returns:
            {
                "started": bool,
                "cache_size": int,
                "cached_strategies": list[str],
                "watcher_status": dict,
                "stats": dict,
            }
        """
        with self._lock:
            cached_names = list(self._cache.keys())

        return {
            "started": self._started,
            "cache_size": len(cached_names),
            "cached_strategies": cached_names,
            "watcher_status": self._watcher.get_status(),
            "stats": {
                "total_reloads": self._stats.total_reloads,
                "successful_reloads": self._stats.successful_reloads,
                "failed_reloads": self._stats.failed_reloads,
                "fallback_to_old_version": self._stats.fallback_to_old_version,
                "total_compiles": self._stats.total_compiles,
                "compile_failures": self._stats.compile_failures,
            },
        }

    def invalidate_cache(self, name: str) -> bool:
        """使指定策略的编译缓存失效.

        Args:
            name: 策略名

        Returns:
            是否成功移除（策略是否在缓存中）
        """
        with self._lock:
            return self._cache.pop(name, None) is not None

    def clear_cache(self) -> int:
        """清空所有编译缓存.

        Returns:
            清除的缓存条目数
        """
        with self._lock:
            count = len(self._cache)
            self._cache.clear()
        return count

    # ------------------------------------------------------------------
    # 内部方法
    # ------------------------------------------------------------------

    def _on_reload_success(self, strategy_name: str) -> None:
        """Watcher 重载成功回调 — 重新编译策略.

        此方法在 watcher 线程中执行。
        """
        logger.info("热重载成功回调: %s，开始重新编译", strategy_name)
        self._recompile_strategy(strategy_name)

    def _on_reload_failure(self, strategy_name: str, error: str) -> None:
        """Watcher 重载失败回调 — 保留旧版本.

        此方法在 watcher 线程中执行。
        重载失败时，旧版本的编译结果仍然有效，无需操作。
        """
        logger.warning(
            "热重载失败回调: %s (error: %s)，保留旧版本编译结果",
            strategy_name,
            error,
        )
        self._stats.fallback_to_old_version += 1

    def _recompile_strategy(self, name: str) -> ReloadResult:
        """重新编译策略并更新缓存.

        如果编译失败，保留旧版本编译结果（降级保护）。

        Args:
            name: 策略名

        Returns:
            ReloadResult
        """
        self._stats.total_compiles += 1

        try:
            entry = StrategyRegistry.get_entry(name)
        except KeyError:
            return ReloadResult(
                strategy_name=name,
                success=False,
                compiled=False,
                error_message=f"策略 '{name}' 未注册",
            )

        # 只有 DSL 策略需要编译
        if not entry.is_dsl:
            # on_bar 策略不需要编译，直接标记成功
            return ReloadResult(
                strategy_name=name,
                success=True,
                compiled=False,  # on_bar 策略无需编译
            )

        # 尝试编译
        try:
            strategy_instance = entry.cls()
            compiled = self._compiler.compile(strategy_instance, name=name)

            with self._lock:
                self._cache[name] = compiled

            self._stats.successful_reloads += 1
            logger.info("策略 '%s' 重新编译成功", name)

            return ReloadResult(
                strategy_name=name,
                success=True,
                compiled=True,
            )

        except Exception as e:
            self._stats.compile_failures += 1
            error_msg = str(e)
            logger.error(
                "策略 '%s' 重新编译失败: %s，保留旧版本",
                name,
                error_msg,
            )

            # 降级保护：保留旧版本
            with self._lock:
                old_kept = name in self._cache

            return ReloadResult(
                strategy_name=name,
                success=False,
                compiled=False,
                error_message=error_msg,
                previous_version_kept=old_kept,
            )

    def _compile_and_cache(self, name: str) -> CompiledStrategy | None:
        """编译策略并加入缓存（缓存未命中时调用）.

        Args:
            name: 策略名

        Returns:
            CompiledStrategy 或 None
        """
        try:
            entry = StrategyRegistry.get_entry(name)
        except KeyError:
            return None

        if not entry.is_dsl:
            return None

        try:
            strategy_instance = entry.cls()
            compiled = self._compiler.compile(strategy_instance, name=name)

            with self._lock:
                self._cache[name] = compiled

            return compiled

        except Exception as e:
            logger.error("策略 '%s' 编译失败: %s", name, e)
            return None

    def _precompile_all(self) -> None:
        """预编译所有已注册的 DSL 策略.

        在 start() 时调用，确保缓存中有所有策略的编译结果。
        """
        dsl_names = StrategyRegistry.list_dsl_strategies()
        if not dsl_names:
            logger.info("无 DSL 策略需要预编译")
            return

        success_count = 0
        for name in dsl_names:
            compiled = self._compile_and_cache(name)
            if compiled is not None:
                success_count += 1
            else:
                logger.warning("预编译策略 '%s' 失败", name)

        logger.info(
            "预编译完成: %d/%d 个 DSL 策略成功",
            success_count,
            len(dsl_names),
        )


class _ReloadStats:
    """重载统计."""

    def __init__(self) -> None:
        self.total_reloads: int = 0
        self.successful_reloads: int = 0
        self.failed_reloads: int = 0
        self.fallback_to_old_version: int = 0
        self.total_compiles: int = 0
        self.compile_failures: int = 0


# ---------------------------------------------------------------------------
# 全局单例
# ---------------------------------------------------------------------------

_manager: HotReloadManager | None = None


def get_hot_reload_manager() -> HotReloadManager:
    """获取全局 HotReloadManager 单例."""
    global _manager
    if _manager is None:
        _manager = HotReloadManager()
    return _manager
