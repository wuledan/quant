#!/usr/bin/env python3
"""模型版本管理."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime


@dataclass
class ModelVersion:
    """模型版本."""

    version_id: str
    model_type: str
    created_at: datetime
    metrics: dict
    config: dict
    feature_list: list[str]
    file_path: str
    is_production: bool = False


class ModelVersioning:
    """模型版本管理."""

    def __init__(self, storage_path: str = "./model_registry") -> None:
        self.storage_path = storage_path
        self._versions: dict[str, ModelVersion] = {}

    def register(self, model, metrics: dict, config: dict, feature_list: list[str]) -> ModelVersion:
        """注册新模型版本."""
        version = ModelVersion(
            version_id=f"v{len(self._versions) + 1}",
            model_type=config.get("model_type", "unknown"),
            created_at=datetime.now(),
            metrics=metrics,
            config=config,
            feature_list=feature_list,
            file_path=f"{self.storage_path}/model_{len(self._versions) + 1}.pkl",
        )
        self._versions[version.version_id] = version
        return version

    def get_production(self, model_type: str) -> ModelVersion | None:
        """获取当前生产版本."""
        for v in self._versions.values():
            if v.model_type == model_type and v.is_production:
                return v
        return None

    def promote(self, version_id: str) -> None:
        """将版本提升为生产版本."""
        if version_id in self._versions:
            self._versions[version_id].is_production = True

    def list_versions(self, model_type: str = "") -> list[ModelVersion]:
        """列出所有版本."""
        if model_type:
            return [v for v in self._versions.values() if v.model_type == model_type]
        return list(self._versions.values())
