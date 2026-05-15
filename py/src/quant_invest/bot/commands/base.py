#!/usr/bin/env python3
"""指令处理器基类."""

from __future__ import annotations

from abc import ABC, abstractmethod


class CommandHandler(ABC):
    """指令处理器基类."""

    @abstractmethod
    async def handle(self, text: str, event: dict) -> dict:
        """处理指令."""
        ...
