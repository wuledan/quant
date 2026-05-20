import { create } from 'zustand';
import { getFactors, getFactorData } from '../api/factor';
import type { FactorDataPoint } from '../api/factor';

interface FactorState {
  factors: string[];
  selectedFactor: string | null;
  factorData: FactorDataPoint[];
  loading: boolean;
  error: string | null;
  fetchFactors: () => Promise<void>;
  fetchFactorData: (symbol: string) => Promise<void>;
  setSelectedFactor: (factor: string | null) => void;
}

export const useFactorStore = create<FactorState>()((set) => ({
  factors: [],
  selectedFactor: null,
  factorData: [],
  loading: false,
  error: null,

  fetchFactors: async () => {
    set({ loading: true, error: null });
    try {
      const list = await getFactors();
      set({ factors: list, loading: false });
    } catch (err) {
      set({ error: err instanceof Error ? err.message : '获取因子列表失败', loading: false });
    }
  },

  fetchFactorData: async (symbol: string) => {
    set({ loading: true, error: null, selectedFactor: symbol });
    try {
      const data = await getFactorData(symbol);
      set({ factorData: data, loading: false });
    } catch (err) {
      set({ error: err instanceof Error ? err.message : '获取因子数据失败', loading: false });
    }
  },

  setSelectedFactor: (factor) => set({ selectedFactor: factor }),
}));
