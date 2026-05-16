"""C++/Rust绑定模块 — pybind11 / Arrow IPC 互操作.

加载编译好的 _quant_core 原生扩展模块，提供:
- 存储引擎: StorageEngine, TimeSeriesStore, ColumnBlock
- 因子引擎: FactorComputer, FactorRegistry, BuiltInFactors
- 执行引擎: OrderManager, OrderStateMachine, MockBroker
- 风控引擎: RiskEngine, RiskRule, RiskAlertPublisher
- 零拷贝传输: shm共享内存池
"""

from __future__ import annotations

import warnings

try:
    from quant_invest import _quant_core as _native

    # Re-export all enums
    from quant_invest._quant_core import (  # noqa: F401
        AlertSeverity,
        Codec,
        ConnectionStatus,
        DataField,
        DataType,
        OrderSide,
        OrderStatus,
        OrderType,
        StoreStatus,
        TimeInForce,
    )

    # Re-export storage
    from quant_invest._quant_core import (  # noqa: F401
        ColumnBlock,
        KlineRow,
        StorageEngine,
        StorageEngineOptions,
        TimeRange,
        TimeSeriesStore,
    )

    # Re-export factor
    from quant_invest._quant_core import (  # noqa: F401
        BuiltInFactors,
        ComputeResult,
        DAGValidationResult,
        FactorComputer,
        FactorDAG,
        FactorMeta,
        FactorRegistry,
    )

    # Re-export execution
    from quant_invest._quant_core import (  # noqa: F401
        AlgoOrderConfig,
        AlgoTraderStats,
        BrokerConfig,
        FillReport,
        MockBroker,
        Order,
        OrderManager,
        OrderRequest,
        OrderStateMachine,
    )

    # Re-export risk
    from quant_invest._quant_core import (  # noqa: F401
        CircuitBreakerConfig,
        ConcentrationRule,
        ExposureRule,
        IRiskRule,
        LimitRule,
        MaxDrawdownRule,
        RiskAlert,
        RiskAlertPublisher,
        RiskCheckResult,
        RiskCheckResultSet,
        RiskContext,
        RiskEngine,
        RiskEngineStats,
    )

    # Zero-copy utilities
    from quant_invest._quant_core import (  # noqa: F401
        numpy_to_vector,
        shm,
        vector_to_numpy,
    )

    __all__ = [name for name in dir() if not name.startswith("_")]

except ImportError as e:
    warnings.warn(f"C++ native module not available: {e}")
    __all__: list[str] = []
