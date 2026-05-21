import { create } from 'zustand';
import type { Strategy, StrategyCreatePayload, StrategyUpdatePayload, BacktestResult } from '../api/strategy';
import * as strategyApi from '../api/strategy';

interface StrategyState {
  strategies: Strategy[];
  currentStrategy: Strategy | null;
  loading: boolean;
  error: string | null;

  fetchStrategies: () => Promise<void>;
  fetchStrategy: (id: number) => Promise<void>;
  createStrategy: (data: StrategyCreatePayload) => Promise<Strategy>;
  updateStrategy: (id: number, data: StrategyUpdatePayload) => Promise<void>;
  deleteStrategy: (id: number) => Promise<void>;
  activateStrategy: (id: number) => Promise<void>;
  pauseStrategy: (id: number) => Promise<void>;
  triggerBacktest: (id: number) => Promise<BacktestResult>;
  getBacktestHistory: (id: number) => Promise<BacktestResult[]>;
  cloneStrategy: (id: number) => Promise<Strategy>;
  clearError: () => void;
}

export const useStrategyStore = create<StrategyState>((set) => ({
  strategies: [],
  currentStrategy: null,
  loading: false,
  error: null,

  fetchStrategies: async () => {
    set({ loading: true, error: null });
    try {
      const strategies = await strategyApi.getStrategies();
      set({ strategies, loading: false });
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch strategies';
      set({ error: msg, loading: false });
    }
  },

  fetchStrategy: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const currentStrategy = await strategyApi.getStrategy(id);
      set({ currentStrategy, loading: false });
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to fetch strategy';
      set({ error: msg, loading: false });
    }
  },

  createStrategy: async (data: StrategyCreatePayload) => {
    set({ loading: true, error: null });
    try {
      const strategy = await strategyApi.createStrategy(data);
      set((state) => ({
        strategies: [...state.strategies, strategy],
        loading: false,
      }));
      return strategy;
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to create strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  updateStrategy: async (id: number, data: StrategyUpdatePayload) => {
    set({ loading: true, error: null });
    try {
      const updated = await strategyApi.updateStrategy(id, data);
      set((state) => ({
        strategies: state.strategies.map((s) => (s.id === id ? updated : s)),
        currentStrategy: state.currentStrategy?.id === id ? updated : state.currentStrategy,
        loading: false,
      }));
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to update strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  deleteStrategy: async (id: number) => {
    set({ loading: true, error: null });
    try {
      await strategyApi.deleteStrategy(id);
      set((state) => ({
        strategies: state.strategies.filter((s) => s.id !== id),
        currentStrategy: state.currentStrategy?.id === id ? null : state.currentStrategy,
        loading: false,
      }));
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to delete strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  activateStrategy: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const updated = await strategyApi.activateStrategy(id);
      set((state) => ({
        strategies: state.strategies.map((s) => (s.id === id ? updated : s)),
        currentStrategy: state.currentStrategy?.id === id ? updated : state.currentStrategy,
        loading: false,
      }));
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to activate strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  pauseStrategy: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const updated = await strategyApi.pauseStrategy(id);
      set((state) => ({
        strategies: state.strategies.map((s) => (s.id === id ? updated : s)),
        currentStrategy: state.currentStrategy?.id === id ? updated : state.currentStrategy,
        loading: false,
      }));
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to pause strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  triggerBacktest: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const result = await strategyApi.triggerBacktest(id);
      set({ loading: false });
      return result;
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to trigger backtest';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  getBacktestHistory: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const history = await strategyApi.getBacktestHistory(id);
      set({ loading: false });
      return history;
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to get backtest history';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  cloneStrategy: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const cloned = await strategyApi.cloneStrategy(id);
      set((state) => ({
        strategies: [...state.strategies, cloned],
        loading: false,
      }));
      return cloned;
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Failed to clone strategy';
      set({ error: msg, loading: false });
      throw err;
    }
  },

  clearError: () => set({ error: null }),
}));