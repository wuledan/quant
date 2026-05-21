import React, { Suspense, lazy } from 'react';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { ConfigProvider, theme } from 'antd';
import Layout from './components/layout/Layout';
import Loading from './components/common/Loading';
import ErrorBoundary from './components/common/ErrorBoundary';
import { useAppStore } from './stores/appStore';

// Lazy-loaded pages
const Dashboard = lazy(() => import('./pages/Dashboard'));
const Settings = lazy(() => import('./pages/Settings'));
const StrategyList = lazy(() => import('./pages/strategies/StrategyList'));
const StrategyCreate = lazy(() => import('./pages/strategies/StrategyCreate'));
const StrategyDetail = lazy(() => import('./pages/strategies/StrategyDetail'));
const StrategyUpload = lazy(() => import('./pages/strategies/StrategyUpload'));
const MarketData = lazy(() => import('./pages/market/MarketData'));
const Backtest = lazy(() => import('./pages/backtest/Backtest'));
const BacktestResult = lazy(() => import('./pages/backtest/BacktestResult'));
const News = lazy(() => import('./pages/news/News'));
const Factors = lazy(() => import('./pages/factors/Factors'));

const SuspenseWrapper: React.FC<{ children: React.ReactNode }> = ({ children }) => (
  <Suspense fallback={<Loading />}>
    <ErrorBoundary>{children}</ErrorBoundary>
  </Suspense>
);

function App() {
  const appTheme = useAppStore((state) => state.theme);

  return (
    <ConfigProvider
      theme={{
        algorithm:
          appTheme === 'dark'
            ? theme.darkAlgorithm
            : theme.defaultAlgorithm,
      }}
    >
      <BrowserRouter>
        <Routes>
          <Route path="/" element={<Layout />}>
            <Route
              index
              element={
                <SuspenseWrapper>
                  <Dashboard />
                </SuspenseWrapper>
              }
            />
            <Route
              path="strategies"
              element={
                <SuspenseWrapper>
                  <StrategyList />
                </SuspenseWrapper>
              }
            />
            <Route
              path="strategies/new"
              element={
                <SuspenseWrapper>
                  <StrategyCreate />
                </SuspenseWrapper>
              }
            />
            <Route
              path="strategies/upload"
              element={
                <SuspenseWrapper>
                  <StrategyUpload />
                </SuspenseWrapper>
              }
            />
            <Route
              path="strategies/:id"
              element={
                <SuspenseWrapper>
                  <StrategyDetail />
                </SuspenseWrapper>
              }
            />
            <Route
              path="market"
              element={
                <SuspenseWrapper>
                  <MarketData />
                </SuspenseWrapper>
              }
            />
            <Route
              path="backtest"
              element={
                <SuspenseWrapper>
                  <Backtest />
                </SuspenseWrapper>
              }
            />
            <Route
              path="backtest/:id/result"
              element={
                <SuspenseWrapper>
                  <BacktestResult />
                </SuspenseWrapper>
              }
            />
            <Route
              path="news"
              element={
                <SuspenseWrapper>
                  <News />
                </SuspenseWrapper>
              }
            />
            <Route
              path="factors"
              element={
                <SuspenseWrapper>
                  <Factors />
                </SuspenseWrapper>
              }
            />
            <Route
              path="settings"
              element={
                <SuspenseWrapper>
                  <Settings />
                </SuspenseWrapper>
              }
            />
          </Route>
        </Routes>
      </BrowserRouter>
    </ConfigProvider>
  );
}

export default App;
