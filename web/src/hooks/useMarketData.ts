import { useEffect, useRef } from 'react';
import { useMarketStore } from '../stores/marketStore';
import { getTicker } from '../api/market';

const POLL_INTERVAL = 5000;

interface UseMarketDataReturn {
  klineData: ReturnType<typeof useMarketStore.getState>['klineData'];
  orderBook: ReturnType<typeof useMarketStore.getState>['orderBook'];
  ticker: ReturnType<typeof useMarketStore.getState>['ticker'];
  loading: boolean;
  error: string | null;
}

export function useMarketData(symbol: string): UseMarketDataReturn {
  const intervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const klineData = useMarketStore((s) => s.klineData);
  const orderBook = useMarketStore((s) => s.orderBook);
  const ticker = useMarketStore((s) => s.ticker);
  const loadingKline = useMarketStore((s) => s.loadingKline);
  const loadingDepth = useMarketStore((s) => s.loadingDepth);
  const klineError = useMarketStore((s) => s.klineError);
  const depthError = useMarketStore((s) => s.depthError);

  const fetchKline = useMarketStore((s) => s.fetchKline);
  const fetchDepth = useMarketStore((s) => s.fetchDepth);
  const setTicker = useMarketStore((s) => s.setTicker);

  useEffect(() => {
    if (!symbol) return;

    fetchKline(symbol);
    fetchDepth(symbol);

    const poll = async () => {
      try {
        const tickerData = await getTicker(symbol);
        setTicker(tickerData);
        fetchDepth(symbol);
      } catch {
        // silent — errors are handled inside store actions
      }
    };

    intervalRef.current = setInterval(poll, POLL_INTERVAL);
    poll();

    return () => {
      if (intervalRef.current !== null) {
        clearInterval(intervalRef.current);
        intervalRef.current = null;
      }
    };
  }, [symbol, fetchKline, fetchDepth, setTicker]);

  const loading = loadingKline || loadingDepth;
  const error = klineError || depthError;

  return { klineData, orderBook, ticker, loading, error };
}
