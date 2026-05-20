"""策略研究框架"""

from .base import StrategyBase, StrategyParams
from .dsl import (
    DAGNode,
    Factor,
    SignalContext,
    SignalExpr,
    Strategy,
    above,
    and_signal,
    below,
    cross_above,
    cross_below,
    get_factor_decls,
    get_signal_decls,
    is_dsl_strategy,
    not_signal,
    or_signal,
    strategy,
    threshold,
)
from .factor_api import FactorAPI
from .position_sizer import PositionSizer
from .registry import StrategyKind, StrategyEntry, StrategyRegistry
from .signal import SignalGenerator

__all__ = [
    # Legacy strategy framework
    "StrategyBase",
    "StrategyParams",
    "SignalGenerator",
    "FactorAPI",
    "StrategyRegistry",
    "StrategyKind",
    "StrategyEntry",
    "PositionSizer",
    # Declarative DSL
    "Strategy",
    "Factor",
    "SignalExpr",
    "DAGNode",
    "SignalContext",
    "strategy",
    "cross_above",
    "cross_below",
    "above",
    "below",
    "and_signal",
    "or_signal",
    "not_signal",
    "threshold",
    "is_dsl_strategy",
    "get_factor_decls",
    "get_signal_decls",
]
