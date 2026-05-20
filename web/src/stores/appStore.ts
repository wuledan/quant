import { create } from 'zustand';
import { persist } from 'zustand/middleware';

export interface SystemStatus {
  engine: 'running' | 'stopped' | 'error';
  lastHeartbeat: string | null;
  uptime: number;
  activeStrategies: number;
  error?: string;
}

interface AppState {
  theme: 'light' | 'dark';
  sidebarCollapsed: boolean;
  systemStatus: SystemStatus;

  setTheme: (theme: 'light' | 'dark') => void;
  toggleTheme: () => void;
  setSidebarCollapsed: (collapsed: boolean) => void;
  toggleSidebar: () => void;
  setSystemStatus: (status: Partial<SystemStatus>) => void;
}

export const useAppStore = create<AppState>()(
  persist(
    (set) => ({
      theme: 'dark',
      sidebarCollapsed: false,
      systemStatus: {
        engine: 'stopped',
        lastHeartbeat: null,
        uptime: 0,
        activeStrategies: 0,
      },

      setTheme: (theme) => set({ theme }),
      toggleTheme: () =>
        set((state) => ({
          theme: state.theme === 'light' ? 'dark' : 'light',
        })),
      setSidebarCollapsed: (sidebarCollapsed) => set({ sidebarCollapsed }),
      toggleSidebar: () =>
        set((state) => ({ sidebarCollapsed: !state.sidebarCollapsed })),
      setSystemStatus: (status) =>
        set((state) => ({
          systemStatus: { ...state.systemStatus, ...status },
        })),
    }),
    {
      name: 'app-store',
      partialize: (state) => ({
        theme: state.theme,
        sidebarCollapsed: state.sidebarCollapsed,
      }),
    }
  )
);
