#!/usr/bin/env python3
"""策略文件监视器 — 监视策略目录变更，触发热重载.

基于 watchdog 库监视策略目录（及子目录）的 .py 文件变更，
当文件发生修改时，自动触发 StrategyRegistry.reload() 重载
对应策略模块，并通过回调通知上层（HotReloadManager）。

设计要点:
- 使用 watchdog.observers.Observer 在后台线程监视文件系统
- 防抖机制避免编辑器保存产生的重复事件（atomic save 等）
- 文件路径 → 策略名映射，支持增量刷新
- 成功/失败回调分离，方便上层（HotReloadManager）做不同处理

用法::

    from quant_invest.strategy.watcher import StrategyWatcher

    watcher = StrategyWatcher()
    watcher.on_reload_success(lambda name: print(f"重载成功: {name}"))
    watcher.on_reload_failure(lambda name, err: print(f"重载失败: {name}: {err}"))
    watcher.start()   # 后台线程启动
    ...
    watcher.stop()    # 停止监视
"""

from __future__ import annotations

import logging
import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from watchdog.events import FileSystemEvent, FileSystemEventHandler
from watchdog.observers import Observer

from .registry import StrategyRegistry

logger = logging.getLogger("quant_invest.strategy.watcher")

# 回调类型定义
ReloadSuccessCallback = Callable[[str], None]
ReloadFailureCallback = Callable[[str, str], None]


@dataclass
class ReloadRecord:
    """重载记录."""

    strategy_name: str
    file_path: str
    success: bool
    error_message: str = ""
    timestamp: float = 0.0


class _StrategyFileHandler(FileSystemEventHandler):
    """策略文件事件处理器 — 响应 .py 文件变更.

    仅处理 Modified 和 Created 事件。使用防抖机制避免编辑器
    保存时产生的多次事件（如 atomic save: write temp → rename）。
    """

    def __init__(
        self,
        watch_dir: str,
        on_file_changed: Callable[[str], None],
        debounce_seconds: float = 0.5,
    ) -> None:
        super().__init__()
        self._watch_dir = watch_dir
        self._on_file_changed = on_file_changed
        self._debounce_seconds = debounce_seconds
        self._pending: dict[str, float] = {}  # file_path → event_time
        self._debounce_timer: threading.Timer | None = None
        self._lock = threading.Lock()

    def on_modified(self, event: FileSystemEvent) -> None:
        """文件修改事件处理."""
        if not self._should_handle(event):
            return
        logger.debug("检测到文件变更: %s", event.src_path)
        self._schedule_debounce(event.src_path)

    def on_created(self, event: FileSystemEvent) -> None:
        """文件创建事件处理 — 新策略文件也触发重载."""
        if not self._should_handle(event):
            return
        logger.debug("检测到新文件: %s", event.src_path)
        self._schedule_debounce(event.src_path)

    def _should_handle(self, event: FileSystemEvent) -> bool:
        """判断事件是否需要处理."""
        if event.is_directory:
            return False
        if not event.src_path.endswith(".py"):
            return False
        # 忽略 __pycache__
        if "__pycache__" in event.src_path:
            return False
        # 忽略临时文件
        basename = os.path.basename(event.src_path)
        if basename.startswith(".") or basename.endswith("~") or basename.endswith(".swp"):
            return False
        # 忽略 __init__.py（修改 __init__.py 通常不是策略变更）
        if basename == "__init__.py":
            return False
        return True

    def _schedule_debounce(self, file_path: str) -> None:
        """防抖调度 — 同一文件在短时间内多次变更只触发一次重载."""
        with self._lock:
            self._pending[file_path] = time.monotonic()
            if self._debounce_timer is not None:
                self._debounce_timer.cancel()
            self._debounce_timer = threading.Timer(
                self._debounce_seconds, self._flush_pending
            )
            self._debounce_timer.daemon = True
            self._debounce_timer.start()

    def _flush_pending(self) -> None:
        """执行防抖队列中所有待处理的重载."""
        with self._lock:
            pending = dict(self._pending)
            self._pending.clear()
            self._debounce_timer = None

        for file_path in sorted(pending.keys()):
            try:
                self._on_file_changed(file_path)
            except Exception as e:
                logger.error("处理文件变更回调异常 %s: %s", file_path, e)

    def cancel(self) -> None:
        """取消待处理的防抖定时器."""
        with self._lock:
            if self._debounce_timer is not None:
                self._debounce_timer.cancel()
                self._debounce_timer = None
            self._pending.clear()


class StrategyWatcher:
    """策略文件监视器 — 监视策略目录变更并触发热重载.

    功能:
    - 监视策略目录及其子目录的 .py 文件变更
    - 文件修改时自动触发 StrategyRegistry.reload()
    - 提供成功/失败回调注册
    - 防抖机制避免编辑器保存产生的重复事件
    - 后台线程运行，不阻塞主线程

    用法::

        watcher = StrategyWatcher()
        watcher.on_reload_success(lambda name: print(f"OK: {name}"))
        watcher.start()
        ...
        watcher.stop()
    """

    def __init__(
        self,
        watch_dir: str | None = None,
        debounce_seconds: float = 0.5,
    ) -> None:
        """初始化策略文件监视器.

        Args:
            watch_dir: 监视目录，默认为策略模块所在目录
            debounce_seconds: 防抖间隔（秒），避免短时间内多次重载
        """
        if watch_dir is None:
            # 默认监视策略模块所在目录
            watch_dir = os.path.dirname(os.path.abspath(__file__))

        self._watch_dir = watch_dir
        self._debounce_seconds = debounce_seconds

        # 回调列表
        self._success_callbacks: list[ReloadSuccessCallback] = []
        self._failure_callbacks: list[ReloadFailureCallback] = []

        # 重载记录
        self._reload_history: list[ReloadRecord] = []
        self._last_reload_times: dict[str, float] = {}  # strategy_name → timestamp

        # 线程安全
        self._lock = threading.Lock()

        # watchdog 组件
        self._observer: Observer | None = None
        self._handler: _StrategyFileHandler | None = None
        self._running = False

        # 文件路径 → 策略名映射缓存
        self._file_to_strategies: dict[str, list[str]] = {}

    @property
    def watch_dir(self) -> str:
        """当前监视的目录."""
        return self._watch_dir

    @property
    def is_running(self) -> bool:
        """监视器是否正在运行."""
        return self._running

    def on_reload_success(self, callback: ReloadSuccessCallback) -> None:
        """注册重载成功回调.

        Args:
            callback: 回调函数，参数为策略名
        """
        with self._lock:
            self._success_callbacks.append(callback)

    def on_reload_failure(self, callback: ReloadFailureCallback) -> None:
        """注册重载失败回调.

        Args:
            callback: 回调函数，参数为策略名和错误信息
        """
        with self._lock:
            self._failure_callbacks.append(callback)

    def start(self) -> None:
        """启动文件监视（后台线程）.

        如果已经在运行，则不做任何操作。
        """
        if self._running:
            logger.warning("StrategyWatcher 已在运行，忽略重复 start()")
            return

        # 初始化文件→策略映射
        self._refresh_file_mapping()

        # 创建 watchdog handler
        self._handler = _StrategyFileHandler(
            watch_dir=self._watch_dir,
            on_file_changed=self._on_file_changed,
            debounce_seconds=self._debounce_seconds,
        )

        # 创建并启动 observer
        self._observer = Observer()
        self._observer.schedule(
            self._handler,
            self._watch_dir,
            recursive=True,  # 递归监视子目录
        )
        self._observer.daemon = True
        self._observer.start()
        self._running = True

        logger.info(
            "StrategyWatcher 已启动，监视目录: %s (递归)",
            self._watch_dir,
        )

    def stop(self) -> None:
        """停止文件监视."""
        if not self._running:
            return

        # 取消防抖定时器
        if self._handler is not None:
            self._handler.cancel()

        # 停止 observer
        if self._observer is not None:
            self._observer.stop()
            self._observer.join(timeout=5.0)
            self._observer = None

        self._handler = None
        self._running = False
        logger.info("StrategyWatcher 已停止")

    def get_status(self) -> dict:
        """获取监视器状态.

        Returns:
            {
                "running": bool,
                "watch_dir": str,
                "watched_files": list[str],
                "last_reload_times": dict[str, float],
                "reload_history_count": int,
                "recent_reloads": list[dict],
            }
        """
        with self._lock:
            last_reload_times = dict(self._last_reload_times)
            recent = self._reload_history[-10:]  # 最近 10 条记录

        # 收集被监视的 .py 文件列表
        watched_files: list[str] = []
        watch_path = Path(self._watch_dir)
        if watch_path.exists():
            for py_file in watch_path.rglob("*.py"):
                rel = str(py_file.relative_to(self._watch_dir))
                if "__pycache__" not in rel:
                    watched_files.append(rel)

        return {
            "running": self._running,
            "watch_dir": self._watch_dir,
            "watched_files": sorted(watched_files),
            "last_reload_times": last_reload_times,
            "reload_history_count": len(self._reload_history),
            "recent_reloads": [
                {
                    "strategy_name": r.strategy_name,
                    "file_path": r.file_path,
                    "success": r.success,
                    "error_message": r.error_message,
                    "timestamp": r.timestamp,
                }
                for r in recent
            ],
        }

    def trigger_reload(self, strategy_name: str) -> bool:
        """手动触发指定策略的重载.

        Args:
            strategy_name: 策略名

        Returns:
            是否重载成功
        """
        return self._do_reload(strategy_name)

    # ------------------------------------------------------------------
    # 内部方法
    # ------------------------------------------------------------------

    def _refresh_file_mapping(self) -> None:
        """刷新文件路径 → 策略名映射.

        遍历 StrategyRegistry 中所有策略，建立
        module_file → [strategy_names] 映射。
        """
        mapping: dict[str, list[str]] = {}
        for name in StrategyRegistry.list_strategies():
            try:
                entry = StrategyRegistry.get_entry(name)
                if entry.module_file:
                    abs_path = os.path.abspath(entry.module_file)
                    if abs_path not in mapping:
                        mapping[abs_path] = []
                    mapping[abs_path].append(name)
            except KeyError:
                pass

        with self._lock:
            self._file_to_strategies = mapping

        logger.debug(
            "文件映射已刷新: %d 个文件, %d 个策略",
            len(mapping),
            sum(len(v) for v in mapping.values()),
        )

    def _on_file_changed(self, file_path: str) -> None:
        """文件变更回调 — 由 _StrategyFileHandler 调用.

        查找变更文件对应的策略名，逐个触发重载。
        """
        abs_path = os.path.abspath(file_path)

        # 刷新映射（可能有新策略注册）
        self._refresh_file_mapping()

        with self._lock:
            strategy_names = self._file_to_strategies.get(abs_path, [])

        if not strategy_names:
            # 文件变更但无对应策略 — 可能是新文件，尝试导入
            logger.info(
                "文件变更但无对应策略: %s，尝试自动导入",
                file_path,
            )
            self._try_import_new_file(file_path)
            return

        logger.info(
            "文件变更触发重载: %s → %s",
            file_path,
            strategy_names,
        )

        for name in strategy_names:
            self._do_reload(name)

    def _try_import_new_file(self, file_path: str) -> None:
        """尝试导入新发现的策略文件.

        将文件路径转换为 Python 模块路径并尝试 import。
        如果成功，新策略会通过 @strategy 装饰器自动注册。
        """
        try:
            # 将文件路径转换为模块路径
            # e.g. /path/to/quant_invest/strategy/examples/new_strat.py
            #   → quant_invest.strategy.examples.new_strat
            rel_path = os.path.relpath(file_path, self._watch_dir)
            if rel_path.startswith(".."):
                # 文件不在策略目录下，跳过
                return

            # 去掉 .py 后缀，将路径分隔符替换为 .
            module_rel = rel_path[:-3].replace(os.sep, ".")

            # 构建完整模块路径
            # 策略目录是 quant_invest.strategy 包的根目录
            module_name = f"quant_invest.strategy.{module_rel}"

            import importlib
            importlib.import_module(module_name)
            logger.info("新策略文件导入成功: %s → %s", file_path, module_name)

            # 刷新映射
            self._refresh_file_mapping()

            # 对新注册的策略触发成功回调
            abs_path = os.path.abspath(file_path)
            with self._lock:
                new_strategies = self._file_to_strategies.get(abs_path, [])

            for name in new_strategies:
                self._notify_success(name)

        except Exception as e:
            logger.warning("新策略文件导入失败: %s: %s", file_path, e)

    def _do_reload(self, strategy_name: str) -> bool:
        """执行策略重载.

        调用 StrategyRegistry.reload() 并通知回调。
        """
        try:
            success = StrategyRegistry.reload(strategy_name)
            now = time.monotonic()

            if success:
                # 刷新映射（重载后策略信息可能变化）
                self._refresh_file_mapping()

                # 获取文件路径
                try:
                    entry = StrategyRegistry.get_entry(strategy_name)
                    file_path = entry.module_file
                except KeyError:
                    file_path = ""

                record = ReloadRecord(
                    strategy_name=strategy_name,
                    file_path=file_path,
                    success=True,
                    timestamp=now,
                )
                with self._lock:
                    self._reload_history.append(record)
                    self._last_reload_times[strategy_name] = now

                self._notify_success(strategy_name)
                logger.info("策略 '%s' 热重载成功", strategy_name)
                return True
            else:
                record = ReloadRecord(
                    strategy_name=strategy_name,
                    file_path="",
                    success=False,
                    error_message="StrategyRegistry.reload() returned False",
                    timestamp=time.monotonic(),
                )
                with self._lock:
                    self._reload_history.append(record)

                self._notify_failure(strategy_name, "reload returned False")
                logger.warning(
                    "策略 '%s' 热重载失败 (reload returned False)",
                    strategy_name,
                )
                return False

        except Exception as e:
            error_msg = str(e)
            record = ReloadRecord(
                strategy_name=strategy_name,
                file_path="",
                success=False,
                error_message=error_msg,
                timestamp=time.monotonic(),
            )
            with self._lock:
                self._reload_history.append(record)

            self._notify_failure(strategy_name, error_msg)
            logger.error("策略 '%s' 热重载异常: %s", strategy_name, e)
            return False

    def _notify_success(self, strategy_name: str) -> None:
        """通知所有成功回调."""
        with self._lock:
            callbacks = list(self._success_callbacks)
        for cb in callbacks:
            try:
                cb(strategy_name)
            except Exception as e:
                logger.error("成功回调异常: %s", e)

    def _notify_failure(self, strategy_name: str, error: str) -> None:
        """通知所有失败回调."""
        with self._lock:
            callbacks = list(self._failure_callbacks)
        for cb in callbacks:
            try:
                cb(strategy_name, error)
            except Exception as e:
                logger.error("失败回调异常: %s", e)


# ---------------------------------------------------------------------------
# 全局单例
# ---------------------------------------------------------------------------

_watcher: StrategyWatcher | None = None


def get_watcher() -> StrategyWatcher:
    """获取全局 StrategyWatcher 单例."""
    global _watcher
    if _watcher is None:
        _watcher = StrategyWatcher()
    return _watcher
