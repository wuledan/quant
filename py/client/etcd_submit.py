# etcd_submit.py — Submit compiled strategies to etcd for C++ engine consumption
#
# The C++ backend watches etcd for new strategies via StrategyWatcher.
# This client compiles a StrategyBase class to IR JSON and writes it
# under /quant/strategy/{strategy_id}/ir and /quant/strategy/{strategy_id}/meta.

from __future__ import annotations

import json
import subprocess
import time
import uuid
from pathlib import Path
from typing import Any

from compiler.ir_compiler import IRCompiler
from dsl.strategy_base import StrategyBase

# Try to import etcd3; fall back to etcdctl subprocess if unavailable
try:
    import etcd3  # type: ignore[import-untyped]

    _HAS_ETCD3 = True
except ImportError:
    _HAS_ETCD3 = False

ETCD_ENDPOINT = "http://127.0.0.1:2379"
ETCDCTL_PATH = "etcdctl"


def _generate_strategy_id() -> str:
    """Generate a unique strategy ID (date-prefixed UUID)."""
    date_part = time.strftime("%Y%m%d")
    short_id = uuid.uuid4().hex[:8]
    return f"{date_part}-{short_id}"


class EtcdSubmitter:
    """Submit compiled strategies to etcd.

    Uses etcd3 library if available; otherwise falls back to etcdctl subprocess
    (consistent with the C++ backend's approach).

    Args:
        endpoints: etcd server endpoints (default: "http://127.0.0.1:2379")
        etcdctl_path: path to etcdctl binary (default: "etcdctl")
    """

    def __init__(
        self,
        endpoints: str = ETCD_ENDPOINT,
        etcdctl_path: str = ETCDCTL_PATH,
    ) -> None:
        self._endpoints = endpoints
        self._etcdctl_path = etcdctl_path
        self._etcd3_client: Any = None

        if _HAS_ETCD3:
            self._etcd3_client = etcd3.client(endpoint=endpoints)
            self._backend = "etcd3"
        else:
            self._backend = "etcdctl"

    @property
    def backend(self) -> str:
        """Which backend is being used (etcd3 or etcdctl)."""
        return self._backend

    def submit_strategy(
        self,
        strategy_cls: type[StrategyBase],
        strategy_name: str | None = None,
    ) -> str:
        """Compile a strategy and submit it to etcd.

        Args:
            strategy_cls: A StrategyBase subclass.
            strategy_name: Optional override name. Defaults to class name.

        Returns:
            strategy_id that can be used to reference the strategy.
        """
        name = strategy_name or getattr(
            strategy_cls, "_strategy_name", strategy_cls.__name__
        )

        # Compile to IR
        compiler = IRCompiler()
        ir_dict = compiler.compile_to_dict(strategy_cls)
        ir_json = json.dumps(ir_dict, indent=2)

        # Generate ID
        strategy_id = _generate_strategy_id()

        # Write IR JSON
        ir_key = f"/quant/strategy/{strategy_id}/ir"
        self._put(ir_key, ir_json)

        # Write metadata
        meta = {
            "strategy_id": strategy_id,
            "strategy_name": name,
            "version": 1,
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "node_count": len(ir_dict["nodes"]),
            "edge_count": len(ir_dict["edges"]),
        }
        meta_key = f"/quant/strategy/{strategy_id}/meta"
        self._put(meta_key, json.dumps(meta, indent=2))

        return strategy_id

    # ── Backend helpers ──

    def _put(self, key: str, value: str) -> None:
        """Write a key-value pair to etcd."""
        if self._etcd3_client is not None:
            self._etcd3_client.put(key, value.encode("utf-8"))
        else:
            self._run_etcdctl(["put", key, value])

    def _get(self, key: str) -> str | None:
        """Read a key from etcd."""
        if self._etcd3_client is not None:
            result = self._etcd3_client.get(key)
            if result is not None and len(result) > 0:
                val, _meta = result  # type: ignore[misc]
                if isinstance(val, bytes):
                    return val.decode("utf-8")
                return str(val)
            return None
        else:
            output = self._run_etcdctl(["get", key, "--print-value-only"])
            return output if output else None

    def _run_etcdctl(self, args: list[str]) -> str:
        """Run an etcdctl command and return stdout."""
        cmd = [self._etcdctl_path, "--endpoints", self._endpoints] + args
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"etcdctl {' '.join(args)} failed: {result.stderr.strip()}"
            )
        return result.stdout.strip()


# ── Convenience function ──

def submit_strategy(
    strategy_cls: type[StrategyBase],
    strategy_name: str | None = None,
    endpoints: str = ETCD_ENDPOINT,
) -> str:
    """One-shot convenience: compile + submit to etcd.

    Args:
        strategy_cls: A StrategyBase subclass.
        strategy_name: Optional strategy name.
        endpoints: etcd endpoint.

    Returns:
        strategy_id.
    """
    submitter = EtcdSubmitter(endpoints=endpoints)
    return submitter.submit_strategy(strategy_cls, strategy_name)


def load_strategy(strategy_id: str, endpoints: str = ETCD_ENDPOINT) -> dict | None:
    """Load a compiled strategy from etcd by ID."""
    submitter = EtcdSubmitter(endpoints=endpoints)
    ir_raw = submitter._get(f"/quant/strategy/{strategy_id}/ir")
    if ir_raw is None:
        return None
    return json.loads(ir_raw)


def list_strategies(endpoints: str = ETCD_ENDPOINT) -> list[dict]:
    """List all strategies registered in etcd."""
    submitter = EtcdSubmitter(endpoints=endpoints)
    prefix = "/quant/strategy/"
    if submitter._etcd3_client is not None:
        results = submitter._etcd3_client.get_prefix(prefix)
        strategies: list[dict] = []
        for val, meta in results:  # type: ignore[misc]
            key = meta.key.decode("utf-8") if isinstance(meta.key, bytes) else str(meta.key)
            strategies.append({"key": key, "value": val.decode("utf-8") if isinstance(val, bytes) else str(val)})
        return strategies
    else:
        output = submitter._run_etcdctl(["get", prefix, "--prefix", "--print-value-only"])
        if not output:
            return []
        return [{"value": output}]
