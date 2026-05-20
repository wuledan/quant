export interface NewsItem {
  id: number;
  title: string;
  summary: string;
  content?: string;
  source: string;
  category: string;
  published_at: string;
  sentiment?: 'positive' | 'negative' | 'neutral';
  created_at?: string;
  updated_at?: string;
}

export interface NewsListResponse {
  data: NewsItem[];
  total: number;
  page: number;
  limit: number;
}

export async function getNews(page = 1, limit = 20): Promise<NewsListResponse> {
  const res = await fetch(`/api/v1/news?page=${page}&limit=${limit}`);
  if (!res.ok) {
    throw new Error(`获取新闻列表失败 (${res.status})`);
  }
  return res.json();
}

export async function getNewsDetail(id: number): Promise<NewsItem> {
  const res = await fetch(`/api/v1/news/${id}`);
  if (!res.ok) {
    throw new Error(`获取新闻详情失败 (${res.status})`);
  }
  return res.json();
}
