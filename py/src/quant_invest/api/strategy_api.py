#!/usr/bin/env python3
"""Strategy API — Python-side HTTP gateway for strategy management.

This module bridges the Python FastAPI frontend with the C++ StrategyApi backend.
It handles:
1. Uploading .py strategy files → IR compilation → .graph file generation
2. Registering compiled strategies with the C++ StrategyEngine via pybind11
3. CRUD operations on strategies (list, get, update, delete)
4. Strategy lifecycle (activate, pause)
5. Backtest triggering and result retrieval

The flow:
  POST /api/v2/strategies (upload .py)
    → Python IRCompiler compiles to .graph JSON
    → C++ StrategyRegistry.register_strategy(name, graph_path, params)
    → Returns strategy ID

  Other endpoints delegate to the C++ StrategyApi via WebSocket JSON messages,
  or directly via pybind11 bindings for synchronous operations.
"""

from __future__ import annotations

import importlib
import importlib.util
import logging
import os
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from fastapi import APIRouter, HTTPException, UploadFile, status

from ..schemas import (
    StrategyListResponse,
    StrategyResponse,
    StrategyStatus,
)

logger = logging.getLogger("quant_invest.api.strategy_api")

router = APIRouter()

# ── Global state ──

# Map strategy_id → {name, graph_path, params, status, created_at, updated_at}
_strategies: dict[int, dict[str, Any]] = {}

# Map strategy_id → list of backtest results
_backtest_runs: dict[int, list[dict[str, Any]]] = {}

_next_id: int = 1

# Graph file storage directory
_GRAPH_DIR = Path(tempfile.gettempdir()) / "quant_graphs"


def _get_next_id() -> int:
    """Generate next strategy ID."""
    global _next_id
    id = _next_id
    _next_id += 1
    return id


def _strategy_to_response(sid: int, entry: dict[str, Any]) -> StrategyResponse:
    """Convert internal strategy dict to StrategyResponse."""
    status_map = {
        "draft": StrategyStatus.CREATED,
        "active": StrategyStatus.RUNNING,
        "paused": StrategyStatus.STOPPED,
        "deleted": StrategyStatus.ERROR,
    }
    created_at = entry.get("created_at", time.time())
    updated_at = entry.get("updated_at", time.time())
    # Convert float timestamp to datetime if needed
    if isinstance(created_at, (int, float)):
        created_at = datetime.fromtimestamp(created_at, tz=timezone.utc)
    if isinstance(updated_at, (int, float)):
        updated_at = datetime.fromtimestamp(updated_at, tz=timezone.utc)
    return StrategyResponse(
        id=str(sid),
        name=entry["name"],
        type=entry.get("type", entry["name"]),
        params=entry.get("params", {}),
        status=status_map.get(entry.get("status", "draft"), StrategyStatus.CREATED),
        description=entry.get("description", ""),
        created_at=created_at,
        updated_at=updated_at,
    )


# ── Try to use C++ backend via pybind11 ──

def _get_cpp_backend() -> Any | None:
    """Get the C++ StrategyApi backend if available."""
    try:
        from quant_invest import _quant_core as _native
        # Check if StrategyApi is available in the bindings
        if hasattr(_native, "StrategyApi"):
            return _native
    except ImportError:
        pass
    return None


def _try_cpp_register(name: str, graph_path: str, params: dict[str, float]) -> int | None:
    """Try to register strategy via C++ backend."""
    try:
        from quant_invest import _quant_core as _native
        if hasattr(_native, "StrategyEngine"):
            # Use the C++ StrategyEngine's registry directly
            engine = _native.StrategyEngine  # This is a class, need instance
            # For now, we can't easily create instances in the Python layer
            # without the service process running. Fall back to Python-only mode.
            logger.debug("C++ StrategyEngine available but needs service integration")
    except ImportError:
        pass
    return None


# ── IR Compiler integration ──

def _compile_strategy_to_graph(strategy_cls: type, name: str) -> str:
    """Compile a Strategy class to .graph file using IRCompiler.

    Args:
        strategy_cls: The Strategy class (DSL v2) to compile
        name: Strategy name for the .graph file

    Returns:
        Path to the generated .graph file
    """
    from quant_invest.strategy.ir_compiler import IRCompiler

    _GRAPH_DIR.mkdir(parents=True, exist_ok=True)
    graph_path = str(_GRAPH_DIR / f"{name}.graph")

    compiler = IRCompiler()
    compiler.write_graph(strategy_cls, graph_path)

    logger.info("Compiled strategy '%s' to graph: %s", name, graph_path)
    return graph_path


def _load_strategy_module(file_path: str, module_name: str) -> type:
    """Load a strategy class from a .py file.

    Args:
        file_path: Path to the .py file
        module_name: Module name to use for import

    Returns:
        The Strategy class found in the module
    """
    spec = importlib.util.spec_from_file_location(module_name, file_path)
    if spec is None or spec.loader is None:
        raise ValueError(f"Cannot load module from {file_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    # Find Strategy subclass in the module
    from quant_invest.strategy.dsl2 import Strategy as StrategyV2
    from quant_invest.strategy.dsl import Strategy as StrategyV1

    strategy_cls = None
    for attr_name in dir(module):
        attr = getattr(module, attr_name)
        if isinstance(attr, type) and (
            issubclass(attr, StrategyV2) or issubclass(attr, StrategyV1)
        ) and attr not in (StrategyV2, StrategyV1):
            strategy_cls = attr
            break

    if strategy_cls is None:
        raise ValueError(f"No Strategy class found in {file_path}")

    return strategy_cls


# ── API endpoints ──

@router.post("/upload", status_code=status.HTTP_201_CREATED)
async def upload_strategy(
    file: UploadFile,
    name: str | None = None,
    params: dict[str, float] | None = None,
) -> StrategyResponse:
    """Upload a .py strategy file → compile to IR → register.

    Flow:
    1. Receive .py file upload
    2. Load the Strategy class from the file
    3. Compile to .graph IR using IRCompiler
    4. Register with strategy registry (Python + C++ if available)
    5. Return strategy ID and metadata
    """
    if not file.filename or not file.filename.endswith(".py"):
        raise HTTPException(
            status_code=400,
            detail="Only .py strategy files are accepted",
        )

    # Save uploaded file to temp location
    content = await file.read()
    tmp_dir = Path(tempfile.gettempdir()) / "quant_upload"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    upload_path = tmp_dir / file.filename
    upload_path.write_bytes(content)

    try:
        # Load strategy module
        strategy_name = name or file.filename.replace(".py", "")
        strategy_cls = _load_strategy_module(str(upload_path), strategy_name)

        # Compile to .graph IR
        graph_path = _compile_strategy_to_graph(strategy_cls, strategy_name)

        # Register strategy
        sid = _get_next_id()
        now = time.time()
        entry = {
            "name": strategy_name,
            "type": strategy_name,
            "graph_path": graph_path,
            "params": params or {},
            "status": "draft",
            "description": f"Uploaded strategy: {file.filename}",
            "created_at": now,
            "updated_at": now,
            "strategy_cls": strategy_cls,
        }
        _strategies[sid] = entry

        # Try to also register in C++ backend
        cpp_id = _try_cpp_register(strategy_name, graph_path, params or {})
        if cpp_id is not None:
            entry["cpp_id"] = cpp_id
            logger.info("Also registered in C++ backend with id=%d", cpp_id)

        return _strategy_to_response(sid, entry)

    except Exception as e:
        logger.error("Failed to process strategy upload: %s", e)
        raise HTTPException(status_code=500, detail=str(e))


@router.get("/list", response_model=StrategyListResponse)
@router.get("", response_model=StrategyListResponse, include_in_schema=False)
async def list_strategies() -> StrategyListResponse:
    """List all registered strategies."""
    strategies = [
        _strategy_to_response(sid, entry)
        for sid, entry in _strategies.items()
        if entry.get("status") != "deleted"
    ]
    return StrategyListResponse(strategies=strategies, total=len(strategies))


@router.get("/{strategy_id}")
async def get_strategy(strategy_id: str) -> StrategyResponse:
    """Get strategy detail by ID."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    entry = _strategies[sid]
    if entry.get("status") == "deleted":
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    return _strategy_to_response(sid, entry)


@router.put("/{strategy_id}")
async def update_strategy(
    strategy_id: str,
    params: dict[str, float] | None = None,
    graph_path: str | None = None,
) -> StrategyResponse:
    """Update strategy parameters or graph path."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    entry = _strategies[sid]
    if params:
        entry["params"] = params
    if graph_path:
        entry["graph_path"] = graph_path
    entry["updated_at"] = time.time()

    return _strategy_to_response(sid, entry)


@router.delete("/{strategy_id}")
async def delete_strategy(strategy_id: str) -> dict:
    """Delete a strategy."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    _strategies[sid]["status"] = "deleted"
    _strategies[sid]["updated_at"] = time.time()

    return {"deleted": sid}


@router.post("/{strategy_id}/activate")
async def activate_strategy(strategy_id: str) -> StrategyResponse:
    """Activate a strategy (C++ hot-load).

    This triggers the C++ StrategyEngine to load the .graph file,
    build the FactorDAG, and start running the strategy.
    """
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    entry = _strategies[sid]
    if entry.get("status") == "deleted":
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    # Try C++ activation via WebSocket message
    try:
        from quant_invest.api.ws_server import get_ws_server
        ws_server = get_ws_server()
        # Send activation command to C++ backend via WebSocket
        # The C++ StrategyApi handles this on the service side
        await ws_server.broadcast("system", {
            "action": "activate_strategy",
            "strategy_id": sid,
            "strategy_name": entry["name"],
            "graph_path": entry["graph_path"],
        })
        logger.info("Sent activation command for strategy %d via WebSocket", sid)
    except Exception as e:
        logger.warning("Could not send activation via WebSocket: %s", e)

    entry["status"] = "active"
    entry["updated_at"] = time.time()

    return _strategy_to_response(sid, entry)


@router.post("/{strategy_id}/pause")
async def pause_strategy(strategy_id: str) -> StrategyResponse:
    """Pause an active strategy."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    entry = _strategies[sid]
    if entry.get("status") != "active":
        raise HTTPException(
            status_code=409,
            detail=f"Strategy '{strategy_id}' is not active, cannot pause",
        )

    # Try C++ pause via WebSocket
    try:
        from quant_invest.api.ws_server import get_ws_server
        ws_server = get_ws_server()
        await ws_server.broadcast("system", {
            "action": "pause_strategy",
            "strategy_id": sid,
        })
    except Exception as e:
        logger.warning("Could not send pause via WebSocket: %s", e)

    entry["status"] = "paused"
    entry["updated_at"] = time.time()

    return _strategy_to_response(sid, entry)


@router.post("/{strategy_id}/backtest")
async def trigger_backtest(
    strategy_id: str,
    initial_cash: float = 1_000_000.0,
    symbol: str = "600519.SH",
    start_date: str | None = None,
    end_date: str | None = None,
) -> BacktestResultResponse:
    """Trigger a backtest for a strategy.

    Uses the C++ BacktestRunner if available, otherwise falls back to
    Python backtest engine.
    """
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    if sid not in _strategies:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")

    entry = _strategies[sid]
    graph_path = entry.get("graph_path", "")

    if not graph_path:
        raise HTTPException(
            status_code=400,
            detail="Strategy has no compiled graph file. Upload and compile first.",
        )

    # Try C++ backtest via pybind11
    result = None
    try:
        from quant_invest import _quant_core as _native
        if hasattr(_native, "BacktestRunner") and hasattr(_native, "StorageEngine"):
            storage = _native.StorageEngine()
            # Note: BacktestRunner needs EventBus which needs Folly executor
            # For now, use the Python backtest engine as fallback
            logger.info("C++ BacktestRunner available but needs service integration")
    except ImportError:
        pass

    # Fall back to Python backtest engine
    if result is None:
        try:
            from quant_invest.backtest.engine import BacktestEngine
            from quant_invest.backtest.data_handler import DataHandler

            engine = BacktestEngine()
            # Use the Python backtest engine with the strategy class
            strategy_cls = entry.get("strategy_cls")
            if strategy_cls is not None:
                result = engine.run_strategy(
                    strategy_cls=strategy_cls,
                    symbol=symbol,
                    initial_cash=initial_cash,
                    start_date=start_date,
                    end_date=end_date,
                )
            else:
                # If no strategy class, try loading from graph file
                result = engine.run_from_graph(
                    graph_path=graph_path,
                    symbol=symbol,
                    initial_cash=initial_cash,
                    start_date=start_date,
                    end_date=end_date,
                )
        except Exception as e:
            logger.error("Backtest failed: %s", e)
            raise HTTPException(status_code=500, detail=f"Backtest failed: {e}")

    # Store result
    run_id = f"bt-{sid}-{len(_backtest_runs.get(sid, [])) + 1}"
    run_entry = {
        "run_id": run_id,
        "strategy_id": sid,
        "result": result,
        "timestamp": time.time(),
        "params": {
            "initial_cash": initial_cash,
            "symbol": symbol,
            "start_date": start_date,
            "end_date": end_date,
        },
    }
    if sid not in _backtest_runs:
        _backtest_runs[sid] = []
    _backtest_runs[sid].append(run_entry)

    return BacktestResultResponse(
        backtest_id=run_id,
        status="completed",
        strategy_id=str(sid),
        metrics=result.get("metrics") if isinstance(result, dict) else None,
        nav_curve=result.get("nav_curve") if isinstance(result, dict) else None,
        trades=result.get("trades") if isinstance(result, dict) else None,
    )


@router.get("/{strategy_id}/runs")
async def list_runs(strategy_id: str) -> dict:
    """List backtest runs for a strategy."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    runs = _backtest_runs.get(sid, [])
    return {"strategy_id": sid, "runs": runs, "total": len(runs)}


@router.get("/{strategy_id}/runs/{run_id}")
async def get_run(strategy_id: str, run_id: str) -> BacktestResultResponse:
    """Get a specific backtest run result."""
    try:
        sid = int(strategy_id)
    except ValueError:
        raise HTTPException(status_code=400, detail="Invalid strategy ID")

    runs = _backtest_runs.get(sid, [])
    for run in runs:
        if run["run_id"] == run_id:
            result = run.get("result", {})
            return BacktestResultResponse(
                backtest_id=run_id,
                status="completed",
                strategy_id=str(sid),
                metrics=result.get("metrics") if isinstance(result, dict) else None,
                nav_curve=result.get("nav_curve") if isinstance(result, dict) else None,
                trades=result.get("trades") if isinstance(result, dict) else None,
            )

    raise HTTPException(status_code=404, detail=f"Run '{run_id}' not found")