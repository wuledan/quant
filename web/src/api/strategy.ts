import apiClient from './client';

export interface Strategy {
  id: number;
  name: string;
  type: string;
  params: Record<string, unknown>;
  status: string;
  created_at: string;
  updated_at: string;
}

export interface StrategyCreatePayload {
  name: string;
  type: string;
  params: Record<string, unknown>;
}

export interface StrategyUpdatePayload {
  name?: string;
  type?: string;
  params?: Record<string, unknown>;
}

interface ApiResponse<T> {
  status: string;
  data: T;
}

export async function getStrategies(): Promise<Strategy[]> {
  const resp = await apiClient.get<ApiResponse<Strategy[]>>('/strategy');
  return resp.data.data;
}

export async function getStrategy(id: number): Promise<Strategy> {
  const resp = await apiClient.get<ApiResponse<Strategy>>(`/strategy/${id}`);
  return resp.data.data;
}

export async function createStrategy(data: StrategyCreatePayload): Promise<Strategy> {
  const resp = await apiClient.post<ApiResponse<Strategy>>('/strategy', data);
  return resp.data.data;
}

export async function updateStrategy(id: number, data: StrategyUpdatePayload): Promise<Strategy> {
  const resp = await apiClient.put<ApiResponse<Strategy>>(`/strategy/${id}`, data);
  return resp.data.data;
}

export async function deleteStrategy(id: number): Promise<void> {
  await apiClient.delete(`/strategy/${id}`);
}

export async function runStrategy(id: number): Promise<void> {
  await apiClient.post(`/strategy/${id}/run`);
}
