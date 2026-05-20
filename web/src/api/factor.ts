export interface FactorDataPoint {
  symbol: string;
  factor_name: string;
  value: number;
  date: string;
}

export async function getFactors(): Promise<string[]> {
  const res = await fetch('/api/v1/factor/list');
  if (!res.ok) {
    throw new Error(`获取因子列表失败 (${res.status})`);
  }
  const data = await res.json();
  // API may return { data: [...] } or raw array
  return Array.isArray(data) ? data : data.data ?? [];
}

export async function getFactorData(symbol: string): Promise<FactorDataPoint[]> {
  const res = await fetch(`/api/v1/factor?symbol=${encodeURIComponent(symbol)}`);
  if (!res.ok) {
    throw new Error(`获取因子数据失败 (${res.status})`);
  }
  const data = await res.json();
  return Array.isArray(data) ? data : data.data ?? [];
}
