# 前端开发计划

> 基于 React + Vite + TypeScript

## 页面路由

```
/                    Dashboard
/strategies          StrategyList
/strategies/new      StrategyCreate
/strategies/:id      StrategyDetail
/market              MarketData
/backtest            Backtest
/backtest/:id/result BacktestResult
/news                News
/factors             Factors
/settings            Settings
```

## Phase 1: 基础框架 (blocks 2-5)
- package.json: 添加 react-router-dom, zustand, lightweight-charts
- src/stores/appStore.ts
- src/components/layout/Layout.tsx, Sidebar.tsx, Header.tsx
- src/components/common/Loading.tsx, Empty.tsx, ErrorBoundary.tsx
- src/pages/Dashboard.tsx, Settings.tsx
- src/App.tsx (改造: Router + Layout)

## Phase 2: 策略管理
- src/stores/strategyStore.ts
- src/api/strategy.ts
- src/components/strategy/StrategyCard.tsx, StrategyForm.tsx, StrategyParams.tsx
- src/pages/StrategyList.tsx, StrategyCreate.tsx, StrategyDetail.tsx

## Phase 3: 行情数据
- src/stores/marketStore.ts
- src/api/market.ts, websocket.ts
- src/hooks/useWebSocket.ts, useMarketData.ts
- src/components/market/SymbolSelector.tsx, TickerTape.tsx, OrderBook.tsx
- src/components/charts/KLineChart.tsx
- src/pages/MarketData.tsx

## Phase 4: 回测结果
- src/stores/backtestStore.ts
- src/api/backtest.ts
- src/components/charts/EquityCurve.tsx, BarChart.tsx
- src/components/common/Card.tsx, DataTable.tsx
- src/pages/Backtest.tsx, BacktestResult.tsx

## Phase 5: 资讯/因子
- src/stores/newsStore.ts, factorStore.ts
- src/api/news.ts, factor.ts
- src/components/charts/Heatmap.tsx
- src/components/common/Modal.tsx
- src/pages/News.tsx, Factors.tsx
