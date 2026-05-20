import { create } from 'zustand';
import type { KlineData, OrderBookData, TickerData, SymbolInfo } from '../api/market';
import { getKline, getDepth, getSymbols } from '../api/market';

/* ------------------------------------------------------------------ */
/*  State & Actions                                                   */
/* ------------------------------------------------------------------ */

interface MarketState {
  /* data */
  symbols: SymbolInfo[];
  currentSymbol: string;
  klineData: KlineData[];
  orderBook: OrderBookData | null;
  ticker: TickerData | null;

  /* loading flags */
  loadingSymbols: boolean;
  loadingKline: boolean;
  loadingDepth: boolean;

  /* errors */
  symbolsError: string | null;
  klineError: string | null;
  depthError: string | null;

  /* actions */
  fetchSymbols: () => Promise<void>;
  selectSymbol: (symbol: string) => void;
  fetchKline: (symbol: string, interval?: string) => Promise<void>;
  fetchDepth: (symbol: string) => Promise<void>;
  setTicker: (ticker: TickerData) => void;
  setOrderBook: (orderBook: OrderBookData) => void;
}

export const useMarketStore = create<MarketState>()((set, get) => ({
  /* initial state */
  symbols: [],
  currentSymbol: '',
  klineData: [],
  orderBook: null,
  ticker: null,

  loadingSymbols: false,
  loadingKline: false,
  loadingDepth: false,

  symbolsError: null,
  klineError: null,
  depthError: null,

  /* -- actions -- */

  fetchSymbols: async () => {
    set({ loadingSymbols: true, symbolsError: null });
    try {
      const symbols = await getSymbols();
      set({ symbols, loadingSymbols: false });
      // auto-select first symbol if none selected
      if (!get().currentSymbol && symbols.length > 0) {
        set({ currentSymbol: symbols[0].symbol });
      }
    } catch (err) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch symbols';
      set({ symbolsError: msg, loadingSymbols: false });
    }
  },

  selectSymbol: (symbol: string) => {
    set({ currentSymbol: symbol, klineData: [], orderBook: null, ticker: null });
  },

  fetchKline: async (symbol: string, interval = '1d') => {
    set({ loadingKline: true, klineError: null });
    try {
      const klineData = await getKline(symbol, interval);
      set({ klineData, loadingKline: false });
    } catch (err) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch kline';
      set({ klineError: msg, loadingKline: false });
    }
  },

  fetchDepth: async (symbol: string) => {
    set({ loadingDepth: true, depthError: null });
    try {
      const orderBook = await getDepth(symbol);
      set({ orderBook, loadingDepth: false });
    } catch (err) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch depth';
      set({ depthError: msg, loadingDepth: false });
    }
  },

  setTicker: (ticker: TickerData) => {
    set({ ticker });
  },

  setOrderBook: (orderBook: OrderBookData) => {
    set({ orderBook });
  },
}));
