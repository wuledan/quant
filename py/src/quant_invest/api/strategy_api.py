#!/usr/bin/env python3
"""Strategy API — Python-side HTTP gateway for strategy management.

All CRUD operations are proxied to the C++ resident service (:9090) via HTTP.
Python only handles:
1. Uploading .py strategy files → IR compilation → .graph JSON generation
2. Forwarding the compiled IR to the C++ service via HTTP POST /api/strategies
3. All other operations (list, get, activate, pause, backtest, etc.) are
   pure HTTP proxies to the C++ StrategyApi.

The flow:
  POST /api/v2/strategies/upload (upload .py)
    → Python IRCompiler compiles to .graph JSON
    → HTTP POST to C++ :9090 /api/strategies with name + graph_content
    → C++ writes ./data/graphs/{name}.graph and registers in StrategyRegistry
    → Returns strategy ID

  All other endpoints → HTTP proxy to C++ :9090 /api/strategies/...
"""

from __future__ import annotations

import importlib
import importlib.util
import json
import logging
import os
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import httpx
from fastapi import APIRouter, HTTPException, UploadFile, status

from .schemas import (
    StrategyListResponse,
    StrategyResponse,
    StrategyStatus,
)

logger = logging.getLogger("quant_invest.api.strategy_api")

router = APIRouter()

# ── C++ service configuration ──

_CPP_SERVICE_HOST = os.environ.get("QUANT_CPP_HOST", "127.0.0.1")
_CPP_SERVICE_PORT = int(os.environ.get("QUANT_CPP_PORT", "9090"))
_CPP_BASE_URL = f"http://{_CPP_SERVICE_HOST}:{_CPP_SERVICE_PORT}"


async def _cpp_request(
    method: str,
    path: str,
    *,
    json_body: dict | None = None,
    timeout: float = 30.0,
) -> httpx.Response:
    """Send an HTTP request to the C++ resident service.

    Raises HTTPException on connection failure.
    """
    url = f"{_CPP_BASE_URL}{path}"
    try:
        async with httpx.AsyncClient(timeout=timeout) as client:
            resp = await client.request(method, url, json=json_body)
            return resp
    except httpx.ConnectError:
        raise HTTPException(
            status_code=503,
            detail=f"C++ service unavailable at {_CPP_BASE_URL}",
        )
    except httpx.TimeoutException:
        raise HTTPException(
            status_code=504,
            detail=f"C++ service timeout at {_CPP_BASE_URL}",
        )


def _cpp_status_to_schema(status_str: str) -> StrategyStatus:
    """Map C++ strategy status to Python schema StrategyStatus."""
    mapping = {
        "draft": StrategyStatus.CREATED,
        "active": StrategyStatus.RUNNING,
        "paused": StrategyStatus.STOPPED,
        "deleted": StrategyStatus.ERROR,
    }
    return mapping.get(status_str, StrategyStatus.CREATED)


def _cpp_entry_to_response(entry: dict) -> StrategyResponse:
    """Convert C++ strategy entry JSON to StrategyResponse."""
    created_at = entry.get("created_at", time.time())
    updated_at = entry.get("updated_at", time.time())
    if isinstance(created_at, (int, float)):
        created_at = datetime.fromtimestamp(created_at, tz=timezone.utc)
    if isinstance(updated_at, (int, float)):
        updated_at = datetime.fromtimestamp(updated_at, tz=timezone.utc)
    return StrategyResponse(
        id=str(entry.get("id", "")),
        name=entry.get("name", ""),
        type=entry.get("name", ""),
        params=entry.get("params", {}),
        status=_cpp_status_to_schema(entry.get("status", "draft")),
        description="",
        created_at=created_at,
        updated_at=updated_at,
    )


# ── IR Compiler integration ──

def _compile_strategy_to_graph(strategy_cls: type, name: str) -> str:
    """Compile a Strategy class to .graph JSON string using IRCompiler.

    Args:
        strategy_cls: The Strategy class (DSL v2) to compile
        name: Strategy name

    Returns:
        The compiled .graph JSON as a string
    """
    from quant_invest.strategy.ir_compiler import IRCompiler

    compiler = IRCompiler()
    ir_dict = compiler.compile(strategy_cls)
    graph_json = json.dumps(ir_dict, indent=2)

    logger.info("Compiled strategy '%s' to graph JSON (%d bytes)", name, len(graph_json))
    return graph_json


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
    """Upload a .py strategy file → compile to IR → register via C++ service.

    Flow:
    1. Receive .py file upload
    2. Load the Strategy class from the file
    3. Compile to .graph IR JSON using IRCompiler
    4. POST to C++ service /api/strategies with name + graph_content
    5. C++ writes ./data/graphs/{name}.graph and registers in StrategyRegistry
    6. Return strategy metadata
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

        # Compile to .graph IR JSON
        graph_json = _compile_strategy_to_graph(strategy_cls, strategy_name)

        # Register with C++ service via HTTP
        resp = await _cpp_request(
            "POST",
            "/api/strategies",
            json_body={
                "name": strategy_name,
                "graph_content": graph_json,
                "params": params or {},
            },
        )

        if resp.status_code not in (200, 201):
            raise HTTPException(
                status_code=resp.status_code,
                detail=resp.json().get("error", "C++ service error"),
            )

        entry = resp.json()
        return _cpp_entry_to_response(entry)

    except HTTPException:
        raise
    except Exception as e:
        logger.error("Failed to process strategy upload: %s", e)
        raise HTTPException(status_code=500, detail=str(e))


@router.post("/register", status_code=status.HTTP_201_CREATED)
async def register_strategy(
    name: str,
    graph_content: str | None = None,
    graph_path: str | None = None,
    params: dict[str, float] | None = None,
) -> StrategyResponse:
    """Register a strategy directly with compiled IR or graph path.

    Either graph_content (JSON string) or graph_path must be provided.
    """
    if not name:
        raise HTTPException(status_code=400, detail="Missing required field: name")

    body: dict[str, Any] = {"name": name, "params": params or {}}
    if graph_content:
        body["graph_content"] = graph_content
    if graph_path:
        body["graph_path"] = graph_path

    resp = await _cpp_request("POST", "/api/strategies", json_body=body)

    if resp.status_code not in (200, 201):
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)


@router.get("/list", response_model=StrategyListResponse)
@router.get("", response_model=StrategyListResponse, include_in_schema=False)
async def list_strategies() -> StrategyListResponse:
    """List all registered strategies (proxied to C++ service)."""
    resp = await _cpp_request("GET", "/api/strategies")

    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    data = resp.json()
    strategies = [_cpp_entry_to_response(s) for s in data.get("strategies", [])]
    return StrategyListResponse(strategies=strategies, total=len(strategies))


@router.get("/{strategy_id}")
async def get_strategy(strategy_id: str) -> StrategyResponse:
    """Get strategy detail by ID (proxied to C++ service)."""
    resp = await _cpp_request("GET", f"/api/strategies/{strategy_id}")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)


@router.put("/{strategy_id}")
async def update_strategy(
    strategy_id: str,
    params: dict[str, float] | None = None,
    graph_path: str | None = None,
) -> StrategyResponse:
    """Update strategy parameters or graph path (proxied to C++ service)."""
    body: dict[str, Any] = {}
    if params:
        body["params"] = params
    if graph_path:
        body["graph_path"] = graph_path

    resp = await _cpp_request("PUT", f"/api/strategies/{strategy_id}", json_body=body)

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)


@router.delete("/{strategy_id}")
async def delete_strategy(strategy_id: str) -> dict:
    """Delete a strategy (proxied to C++ service)."""
    resp = await _cpp_request("DELETE", f"/api/strategies/{strategy_id}")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    return resp.json()


@router.post("/{strategy_id}/activate")
async def activate_strategy(strategy_id: str) -> StrategyResponse:
    """Activate a strategy — C++ hot-loads the .graph file (proxied to C++ service)."""
    resp = await _cpp_request("POST", f"/api/strategies/{strategy_id}/activate")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code == 409:
        raise HTTPException(
            status_code=409,
            detail=f"Cannot activate strategy '{strategy_id}' (already active or invalid graph)",
        )
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)


@router.post("/{strategy_id}/pause")
async def pause_strategy(strategy_id: str) -> StrategyResponse:
    """Pause an active strategy (proxied to C++ service)."""
    resp = await _cpp_request("POST", f"/api/strategies/{strategy_id}/pause")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code == 409:
        raise HTTPException(
            status_code=409,
            detail=f"Cannot pause strategy '{strategy_id}' (not active)",
        )
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)


@router.post("/{strategy_id}/backtest")
async def trigger_backtest(
    strategy_id: str,
    initial_cash: float = 1_000_000.0,
    symbol: str = "600519.SH",
    start_date: str | None = None,
    end_date: str | None = None,
) -> dict:
    """Trigger a backtest for a strategy (proxied to C++ service).

    The C++ BacktestRunner loads the .graph file and runs the backtest.
    """
    body: dict[str, Any] = {
        "initial_cash": initial_cash,
        "symbol": symbol,
    }
    if start_date:
        body["start_date"] = start_date
    if end_date:
        body["end_date"] = end_date

    resp = await _cpp_request(
        "POST",
        f"/api/strategies/{strategy_id}/backtest",
        json_body=body,
        timeout=120.0,  # backtest may take longer
    )

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code == 400:
        raise HTTPException(
            status_code=400,
            detail=resp.json().get("error", "No graph_path configured"),
        )
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    return resp.json()


@router.get("/{strategy_id}/backtest-history")
async def backtest_history(strategy_id: str) -> dict:
    """Get backtest history for a strategy (proxied to C++ service)."""
    resp = await _cpp_request("GET", f"/api/strategies/{strategy_id}/backtest-history")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code != 200:
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    return resp.json()


@router.post("/{strategy_id}/clone")
async def clone_strategy(strategy_id: str) -> StrategyResponse:
    """Clone a strategy (proxied to C++ service)."""
    resp = await _cpp_request("POST", f"/api/strategies/{strategy_id}/clone")

    if resp.status_code == 404:
        raise HTTPException(status_code=404, detail=f"Strategy '{strategy_id}' not found")
    if resp.status_code not in (200, 201):
        raise HTTPException(
            status_code=resp.status_code,
            detail=resp.json().get("error", "C++ service error"),
        )

    entry = resp.json()
    return _cpp_entry_to_response(entry)
