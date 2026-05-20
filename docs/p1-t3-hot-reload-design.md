# P1-T3: 策略热重载设计文档

## 1. 概述

策略热重载系统允许在开发过程中修改策略文件后自动重载，无需重启后端服务。系统由三层组成：

1. **StrategyWatcher** — 文件监视器，基于 watchdog 检测策略文件变更
2. **HotReloadManager** — 热重载管理器，协调文件监视与策略编译
3. **API 路由** — 提供手动重载和状态查询接口

## 2. 架构

```
┌──────────────────────────────────────────────────────┐
│                    API Layer                          │
│  POST /api/v1/strategy/reload/{name}                 │
│  GET  /api/v1/strategy/reload/status                 │
└──────────────────┬───────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────┐
│              HotReloadManager                         │
│  ┌─────────────────────────────────────────────┐     │
│  │  编译缓存 dict[str, CompiledStrategy]        │     │
│  │  - get_compiled(name) → CompiledStrategy     │     │
│  │  - reload_strategy(name) → ReloadResult      │     │
│  │  - 降级保护: 编译失败保留旧版本               │     │
│  └─────────────────────────────────────────────┘     │
│                                                       │
│  on_reload_success → 重新编译 + 更新缓存              │
│  on_reload_failure → 保留旧版本 + 记录错误            │
└──────────┬───────────────────────┬───────────────────┘
           │                       │
┌──────────▼──────────┐  ┌────────▼──────────────────┐
│  StrategyWatcher     │  │  StrategyCompiler          │
│  (watchdog Observer) │  │  (DSL → FactorDAG)         │
│                      │  │                             │
│  .py 文件变更检测     │  │  compile(instance) →        │
│  → 防抖 (0.5s)      │  │    CompiledStrategy         │
│  → StrategyRegistry  │  │                             │
│    .reload(name)     │  │                             │
│  → 成功/失败回调      │  │                             │
└──────────────────────┘  └─────────────────────────────┘
```

## 3. 文件监视器 (StrategyWatcher)

### 3.1 技术选型

| 方案 | 优势 | 劣势 |
|------|------|------|
| **watchdog** ✅ | 跨平台、线程模型、生态成熟 | 需额外依赖 |
| watchfiles | 异步原生、Rust 实现 | 需要 async 上下文 |
| 轮询 (os.stat) | 零依赖 | 延迟高、CPU 浪费 |

选择 **watchdog** 的原因：
- 后台线程模型，不依赖 async 上下文
- 可在 FastAPI lifespan 中直接 start/stop
- 防抖机制简单（threading.Timer）
- 跨平台兼容性好

### 3.2 核心流程

```
文件变更事件 (watchdog)
    │
    ▼
_StrategyFileHandler.on_modified()
    │
    ├─ 过滤: .py 文件? __pycache__? 临时文件?
    │
    ▼
防抖调度 (_schedule_debounce)
    │
    ├─ 同一文件 0.5s 内多次变更 → 合并为一次
    │
    ▼
_flush_pending()
    │
    ▼
_on_file_changed(file_path)
    │
    ├─ 刷新 文件→策略名 映射
    │
    ├─ 有对应策略? → _do_reload(name) × N
    │                  │
    │                  ├─ StrategyRegistry.reload(name)
    │                  │     ├─ importlib.reload(module)
    │                  │     └─ @strategy 装饰器重新注册
    │                  │
    │                  └─ 通知回调 (success/failure)
    │
    └─ 无对应策略? → _try_import_new_file()
                       │
                       └─ importlib.import_module()
```

### 3.3 防抖机制

编辑器保存文件时可能产生多个事件：
- **VS Code**: write → chmod (2 次事件)
- **Vim**: write temp → rename (2 次事件)
- **PyCharm**: write → safe write (2 次事件)

防抖策略：收到事件后等待 0.5s，期间同一文件的后续事件合并。0.5s 后统一处理。

```python
# 防抖实现
def _schedule_debounce(self, file_path):
    self._pending[file_path] = time.monotonic()
    if self._debounce_timer:
        self._debounce_timer.cancel()
    self._debounce_timer = threading.Timer(0.5, self._flush_pending)
    self._debounce_timer.daemon = True
    self._debounce_timer.start()
```

### 3.4 文件→策略名映射

StrategyRegistry 中每个 StrategyEntry 记录了 `module_file`（策略类所在的 .py 文件路径）。Watcher 维护一个反向映射：

```python
{
    "/abs/path/to/examples/ma_cross_dsl.py": ["ma_cross_dsl", "ma_cross_dsl_simple", "ma_cross_hybrid", "rsi_ma_combo"],
    "/abs/path/to/ma_cross.py": ["ma_cross"],
}
```

一个文件可能包含多个策略（多个 `@strategy` 装饰器），因此映射值是列表。

映射在以下时机刷新：
- watcher.start() 时
- 每次文件变更回调时
- 重载成功后

## 4. 热重载管理器 (HotReloadManager)

### 4.1 职责

HotReloadManager 是文件监视和策略编译之间的协调层：

1. **编译缓存管理**: 维护 `dict[str, CompiledStrategy]` 缓存
2. **重载协调**: 监听 watcher 回调，触发重新编译
3. **降级保护**: 编译失败时保留旧版本
4. **预编译**: 启动时预编译所有已注册的 DSL 策略

### 4.2 降级保护

```
策略文件变更
    │
    ▼
StrategyRegistry.reload(name) ──── 失败 ──→ 保留旧版本，记录错误
    │
    ▼ 成功
StrategyCompiler.compile() ──── 失败 ──→ 保留旧版本，记录错误
    │
    ▼ 成功
更新缓存 cache[name] = new_compiled
```

关键设计：**编译失败不删除旧版本**。这确保正在运行的回测/交易不会因策略文件语法错误而中断。

### 4.3 预编译流程

```python
def _precompile_all(self):
    dsl_names = StrategyRegistry.list_dsl_strategies()
    for name in dsl_names:
        compiled = self._compile_and_cache(name)
```

启动时遍历所有 DSL 策略，编译并缓存。on_bar 策略不需要编译。

### 4.4 线程安全

| 数据 | 保护方式 | 说明 |
|------|----------|------|
| `_cache` | `RLock` | 读多写少，RLock 允许重入 |
| `_stats` | 无锁（单线程写） | 仅在回调中更新 |
| watcher 回调 | watcher 线程 | 回调中操作缓存需加锁 |

## 5. API 路由

### 5.1 手动重载

```
POST /api/v1/strategy/reload/{name}
```

请求示例：
```bash
curl -X POST http://localhost:8000/api/v1/strategy/reload/ma_cross_dsl
```

响应：
```json
{
    "strategy_name": "ma_cross_dsl",
    "success": true,
    "compiled": true,
    "error_message": "",
    "previous_version_kept": false,
    "timestamp": "2026-05-20T10:30:00+00:00"
}
```

### 5.2 状态查询

```
GET /api/v1/strategy/reload/status
```

响应：
```json
{
    "started": true,
    "cache_size": 4,
    "cached_strategies": ["ma_cross_dsl", "ma_cross_dsl_simple", "ma_cross_hybrid", "rsi_ma_combo"],
    "watcher_status": {
        "running": true,
        "watch_dir": "/path/to/strategy",
        "watched_files": ["examples/ma_cross_dsl.py", "ma_cross.py", ...],
        "last_reload_times": {"ma_cross_dsl": 1716201000.0},
        "reload_history_count": 3,
        "recent_reloads": [...]
    },
    "stats": {
        "total_reloads": 5,
        "successful_reloads": 4,
        "failed_reloads": 1,
        "fallback_to_old_version": 1,
        "total_compiles": 4,
        "compile_failures": 0
    }
}
```

### 5.3 路由注册顺序

热重载路由 `/reload/{name}` 和 `/reload/status` 必须在 `/{strategy_id}` 之前注册，否则 FastAPI 会将 `reload` 误匹配为 `strategy_id` 参数。

## 6. 与 FastAPI 集成

### 6.1 Lifespan 集成

```python
# 在 app.py lifespan 中启动/停止
@asynccontextmanager
async def lifespan(app: FastAPI):
    # Startup
    from quant_invest.strategy.hot_reload import get_hot_reload_manager
    manager = get_hot_reload_manager()
    manager.start()
    app.state.hot_reload_manager = manager

    yield

    # Shutdown
    manager.stop()
```

### 6.2 全局单例

- `get_watcher()` → 全局 StrategyWatcher 单例
- `get_hot_reload_manager()` → 全局 HotReloadManager 单例

单例模式确保 API 路由和 lifespan 使用同一个管理器实例。

## 7. 错误处理

### 7.1 错误场景

| 场景 | 处理方式 | 影响 |
|------|----------|------|
| 策略文件语法错误 | importlib.reload() 抛异常 | 旧版本保留，watcher 记录失败 |
| 策略文件逻辑错误 | 编译失败 | 旧版本保留，HotReloadManager 记录 |
| 策略文件删除 | StrategyRegistry.reload() 返回 False | 旧版本保留 |
| 新文件导入失败 | importlib.import_module() 抛异常 | 仅记录日志，不影响已有策略 |
| 编译器 C++ 模块不可用 | 回退到纯 Python 编译 | 功能降级但可用 |

### 7.2 日志级别

| 级别 | 场景 |
|------|------|
| DEBUG | 文件变更检测、映射刷新 |
| INFO | 重载成功、编译成功、启动/停止 |
| WARNING | 重载失败（预期内）、新文件导入失败 |
| ERROR | 回调异常、编译异常 |

## 8. 性能考虑

### 8.1 防抖

0.5s 防抖间隔平衡了响应速度和重载频率：
- 太短（0.1s）: 编辑器多次保存事件可能触发多次重载
- 太长（2s）: 开发体验差，修改后等待时间长

### 8.2 映射刷新

每次文件变更时刷新 文件→策略名 映射，开销很小（遍历注册表，通常 < 10 个策略）。

### 8.3 编译缓存

编译缓存避免每次请求都重新编译。缓存命中时 `get_compiled()` 是 O(1) 字典查找。

## 9. 测试方案

### 9.1 单元测试

```python
# 测试 watcher 防抖
def test_debounce():
    handler = _StrategyFileHandler(...)
    handler.on_modified(event1)  # t=0
    handler.on_modified(event2)  # t=0.1 (same file)
    # 只触发一次回调

# 测试 HotReloadManager 降级保护
def test_fallback_on_compile_failure():
    manager = HotReloadManager()
    manager.start()
    # 缓存旧版本
    old_compiled = manager.get_compiled("ma_cross_dsl")
    # 模拟编译失败
    with patch.object(manager._compiler, 'compile', side_effect=Exception):
        manager._recompile_strategy("ma_cross_dsl")
    # 旧版本仍在缓存中
    assert manager.get_compiled("ma_cross_dsl") is old_compiled
```

### 9.2 集成测试

```python
# 测试端到端热重载
def test_hot_reload_e2e():
    manager = HotReloadManager()
    manager.start()

    # 修改策略文件
    strategy_file = "py/src/quant_invest/strategy/examples/ma_cross_dsl.py"
    with open(strategy_file, "a") as f:
        f.write("\n# test modification\n")

    # 等待 watcher 检测变更
    time.sleep(2.0)

    # 验证重载成功
    status = manager.get_status()
    assert status["stats"]["successful_reloads"] > 0

    manager.stop()
```

## 10. 文件清单

| 文件 | 说明 |
|------|------|
| `py/src/quant_invest/strategy/watcher.py` | 文件监视器（watchdog） |
| `py/src/quant_invest/strategy/hot_reload.py` | 热重载管理器 |
| `py/src/quant_invest/strategy/__init__.py` | 模块导出（新增 watcher/hot_reload） |
| `py/src/quant_invest/api/routes/strategy.py` | API 路由（新增 reload 端点） |
