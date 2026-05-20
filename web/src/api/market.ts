import apiClient from './client';

/* ------------------------------------------------------------------ */
/*  Types                                                             */
/* ------------------------------------------------------------------ */

export interface KlineData {
  timestamp: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
}

export type DepthLevel = [number, number]; // [price, size]

export interface OrderBookData {
  bids: DepthLevel[];
  asks: DepthLevel[];
}

export interface TickerData {
  symbol: string;
  price: number;
  change: number;
  volume: number;
  timestamp: number;
}

export interface SymbolInfo {
  symbol: string;
  name?: string;
  exchange?: string;
}

/* ------------------------------------------------------------------ */
/*  Kline                                                             */
/* ------------------------------------------------------------------ */

export async function getKline(
  symbol: string,
  interval: string,
  start?: number,
  end?: number,
): Promise<KlineData[]> {
  const params: Record<string, string | number> = { symbol, interval };
  if (start !== undefined) params.start = start;
  if (end !== undefined) params.end = end;
  const res = await apiClient.get<KlineData[]>('/data/kline', { params });
  return res.data;
}

/* ------------------------------------------------------------------ */
/*  Depth                                                             */
/* ------------------------------------------------------------------ */

export async function getDepth(symbol: string): Promise<OrderBookData> {
  const res = await apiClient.get<OrderBookData>('/data/depth', {
    params: { symbol },
  });
  return res.data;
}

/* ------------------------------------------------------------------ */
/*  Ticker                                                            */
/* ------------------------------------------------------------------ */

export async function getTicker(symbol: string): Promise<TickerData> {
  const res = await apiClient.get<TickerData>('/data/ticker', {
    params: { symbol },
  });
  return res.data;
}

/* ------------------------------------------------------------------ */
/*  Symbols                                                           */
/* ------------------------------------------------------------------ */

export async function getSymbols(): Promise<SymbolInfo[]> {
  const res = await apiClient.get<SymbolInfo[]>('/symbols');
  return res.data;
}
