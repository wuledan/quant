import { useEffect, useRef, useCallback } from 'react';
import { useMarketStore } from '../stores/marketStore';
import { useWebSocket } from './useWebSocket';
import { getKline, getDepth } from '../api/market';

const WS_URL = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/api/v1/ws`;

export function useMarketData(symbol: string) {
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
  const setOrderBook = useMarketStore((s) => s.setOrderBook);
  const updateKlineBar = useMarketStore((s) => s.updateKlineBar);

  const { isConnected, lastMessage, subscribe } = useWebSocket(WS_URL, ['kline', 'trade']);

  // Subscribe to kline channel when symbol changes
  useEffect(() => {
    if (symbol && isConnected) {
      subscribe('kline');
    }
  }, [symbol, isConnected, subscribe]);

  // Handle WebSocket messages
  useEffect(() => {
    if (!lastMessage) return;

    const { channel, data } = lastMessage as { channel: string; data: Record<string, unknown> };

    if (channel === 'kline' && data) {
      if (data.symbol === symbol) {
        if (data.close !== undefined) {
          setTicker({
            symbol: data.symbol as string,
            price: data.close as number,
            change: (data.change as number) ?? 0,
            volume: (data.volume as number) ?? 0,
            timestamp: Date.now(),
          });
        }
        if (data.open !== undefined && data.high !== undefined) {
          updateKlineBar({
            timestamp: (data.timestamp as number) ?? Date.now(),
            open: data.open as number,
            high: data.high as number,
            low: data.low as number,
            close: data.close as number,
            volume: (data.volume as number) ?? 0,
          });
        }
      }
    } else if (channel === 'trade' && data) {
      if (data.symbol === symbol) {
        setTicker({
          symbol: data.symbol as string,
          price: data.price as number,
          change: (data.change as number) ?? 0,
          volume: (data.volume as number) ?? 0,
          timestamp: Date.now(),
        });
      }
    }
  }, [lastMessage, symbol, setTicker, updateKlineBar]);

  // Initial data fetch via REST API
  const initialFetchRef = useRef(false);
  useEffect(() => {
    if (!symbol) return;
    fetchKline(symbol);
    fetchDepth(symbol);
    initialFetchRef.current = true;
  }, [symbol, fetchKline, fetchDepth]);

  // Periodic depth refresh (order book not pushed via WS yet)
  const depthIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);
  useEffect(() => {
    if (!symbol) return;

    depthIntervalRef.current = setInterval(() => {
      fetchDepth(symbol);
    }, 10000);

    return () => {
      if (depthIntervalRef.current !== null) {
        clearInterval(depthIntervalRef.current);
        depthIntervalRef.current = null;
      }
    };
  }, [symbol, fetchDepth]);

  const loading = loadingKline || loadingDepth;
  const error = klineError || depthError;

  return { klineData, orderBook, ticker, loading, error, wsConnected: isConnected };
}
