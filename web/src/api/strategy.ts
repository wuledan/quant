import apiClient, { pythonClient } from './client';

/* ---------- Types ---------- */

export interface Strategy {
  id: number;
  name: string;
  status: 'draft' | 'active' | 'paused' | 'deleted';
  graph_path?: string;
  params: Record<string, unknown>;
  created_at: string;
  updated_at: string;
}

export interface StrategyCreatePayload {
  name: string;
  graph_content?: string;
  graph_path?: string;
  params?: Record<string, unknown>;
}

export interface StrategyUpdatePayload {
  name?: string;
  graph_content?: string;
  graph_path?: string;
  params?: Record<string, unknown>;
}

export interface NavPoint {
  timestamp: number;
  nav: number;
}

export interface BacktestResult {
  total_return: number;
  annual_return: number;
  max_drawdown: number;
  sharpe_ratio: number;
  total_trades: number;
  nav_curve: NavPoint[];
}

/* ---------- C++ StrategyApi REST endpoints ---------- */

export async function getStrategies(): Promise<Strategy[]> {
  const resp = await apiClient.get<Strategy[]>('/strategies');
  return resp.data;
}

export async function getStrategy(id: number): Promise<Strategy> {
  const resp = await apiClient.get<Strategy>(`/strategies/${id}`);
  return resp.data;
}

export async function createStrategy(data: StrategyCreatePayload): Promise<Strategy> {
  const resp = await apiClient.post<Strategy>('/strategies', data);
  return resp.data;
}

export async function updateStrategy(id: number, data: StrategyUpdatePayload): Promise<Strategy> {
  const resp = await apiClient.put<Strategy>(`/strategies/${id}`, data);
  return resp.data;
}

export async function deleteStrategy(id: number): Promise<void> {
  await apiClient.delete(`/strategies/${id}`);
}

export async function activateStrategy(id: number): Promise<Strategy> {
  const resp = await apiClient.post<Strategy>(`/strategies/${id}/activate`);
  return resp.data;
}

export async function pauseStrategy(id: number): Promise<Strategy> {
  const resp = await apiClient.post<Strategy>(`/strategies/${id}/pause`);
  return resp.data;
}

export async function triggerBacktest(id: number): Promise<BacktestResult> {
  const resp = await apiClient.post<BacktestResult>(`/strategies/${id}/backtest`);
  return resp.data;
}

export async function getBacktestHistory(id: number): Promise<BacktestResult[]> {
  const resp = await apiClient.get<BacktestResult[]>(`/strategies/${id}/backtest-history`);
  return resp.data;
}

export async function cloneStrategy(id: number): Promise<Strategy> {
  const resp = await apiClient.post<Strategy>(`/strategies/${id}/clone`);
  return resp.data;
}

/* ---------- Python FastAPI — upload .py for IR compilation ---------- */

export interface UploadResponse {
  strategy_id: number;
  name: string;
  status: string;
  message?: string;
}

export async function uploadPythonFile(file: File): Promise<UploadResponse> {
  const formData = new FormData();
  formData.append('file', file);
  const resp = await pythonClient.post<UploadResponse>('/strategies/upload', formData, {
    headers: { 'Content-Type': 'multipart/form-data' },
  });
  return resp.data;
}
