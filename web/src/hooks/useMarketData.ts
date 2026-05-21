import { useState, useEffect, useCallback } from 'react';

export interface KlineData {
  timestamp: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

export interface MarketQuote {
  symbol: string;
  price: number;
  change: number;
  changePercent: number;
  volume: number;
  timestamp: number;
}

interface UseMarketDataReturn {
  quotes: MarketQuote[];
  klineData: KlineData[];
  loading: boolean;
  error: string | null;
  refresh: () => void;
}

// TODO: Replace mock data with real API calls when market data endpoint is available
const MOCK_QUOTES: MarketQuote[] = [
  { symbol: '000001.SZ', price: 12.34, change: 0.12, changePercent: 0.98, volume: 1234567, timestamp: Date.now() },
  { symbol: '600519.SH', price: 1856.00, change: -12.50, changePercent: -0.67, volume: 89012, timestamp: Date.now() },
  { symbol: '000858.SZ', price: 156.78, change: 2.34, changePercent: 1.52, volume: 345678, timestamp: Date.now() },
];

const MOCK_KLINE: KlineData[] = Array.from({ length: 30 }, (_, i) => ({
  timestamp: Date.now() - (30 - i) * 86400000,
  open: 12 + Math.random() * 2,
  high: 13 + Math.random() * 2,
  low: 11 + Math.random() * 2,
  close: 12 + Math.random() * 2,
  volume: Math.floor(Math.random() * 1000000),
}));

export const useMarketData = (): UseMarketDataReturn => {
  const [quotes, setQuotes] = useState<MarketQuote[]>([]);
  const [klineData, setKlineData] = useState<KlineData[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  const fetchData = useCallback(() => {
    setLoading(true);
    setError(null);
    // Simulate API call
    setTimeout(() => {
      try {
        setQuotes(MOCK_QUOTES);
        setKlineData(MOCK_KLINE);
        setLoading(false);
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Unknown error');
        setLoading(false);
      }
    }, 500);
  }, []);

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  return { quotes, klineData, loading, error, refresh: fetchData };
};

export default useMarketData;
