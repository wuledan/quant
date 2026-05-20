import { create } from 'zustand';
import { getNews, getNewsDetail } from '../api/news';
import type { NewsItem } from '../api/news';

interface Pagination {
  page: number;
  limit: number;
  total: number;
}

interface NewsState {
  news: NewsItem[];
  currentNews: NewsItem | null;
  loading: boolean;
  error: string | null;
  pagination: Pagination;
  fetchNews: (page?: number, limit?: number) => Promise<void>;
  fetchNewsDetail: (id: number) => Promise<void>;
  clearCurrentNews: () => void;
}

export const useNewsStore = create<NewsState>()((set) => ({
  news: [],
  currentNews: null,
  loading: false,
  error: null,
  pagination: { page: 1, limit: 20, total: 0 },

  fetchNews: async (page = 1, limit = 20) => {
    set({ loading: true, error: null });
    try {
      const response = await getNews(page, limit);
      set({
        news: response.data,
        pagination: { page: response.page, limit: response.limit, total: response.total },
        loading: false,
      });
    } catch (err) {
      set({ error: err instanceof Error ? err.message : '获取新闻列表失败', loading: false });
    }
  },

  fetchNewsDetail: async (id: number) => {
    set({ loading: true, error: null });
    try {
      const detail = await getNewsDetail(id);
      set({ currentNews: detail, loading: false });
    } catch (err) {
      set({ error: err instanceof Error ? err.message : '获取新闻详情失败', loading: false });
    }
  },

  clearCurrentNews: () => set({ currentNews: null }),
}));
