import React, { useEffect, useState } from 'react';
import { Card, List, Tag, Typography, Spin, Empty, Space, Select, Button, Badge, message } from 'antd';
import { ClockCircleOutlined, TagOutlined, ReloadOutlined } from '@ant-design/icons';

const { Title, Text, Paragraph } = Typography;
const API_BASE = '/api/v1';

interface NewsItem {
  id: string;
  title: string;
  content: string;
  source: string;
  published_at: string;
  symbols: string[];
  tags: string[];
  importance: number;
}

const sourceLabel: Record<string, string> = {
  cls: '财联社',
  sina: '新浪财经',
  eastmoney: '东方财富',
};

const tagLabel: Record<string, string> = {
  policy: '政策',
  earnings: '财报',
  macro: '宏观',
  geo: '地缘',
  industry: '行业',
  market: '市场',
};

const tagColor: Record<string, string> = {
  policy: 'blue',
  earnings: 'green',
  macro: 'purple',
  geo: 'orange',
  industry: 'cyan',
  market: 'default',
};

const News: React.FC = () => {
  const [news, setNews] = useState<NewsItem[]>([]);
  const [loading, setLoading] = useState(true);
  const [filter, setFilter] = useState<string>('all');
  const [lastUpdate, setLastUpdate] = useState<string>('');

  const fetchNews = async () => {
    setLoading(true);
    try {
      const res = await fetch(`${API_BASE}/news/latest?count=100`);
      if (!res.ok) throw new Error('Failed to fetch news');
      const data = await res.json();
      setNews(data.news || []);
      setLastUpdate(new Date().toLocaleTimeString('zh-CN'));
    } catch {
      message.error('获取新闻失败');
      setNews([]);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchNews();
    const timer = setInterval(fetchNews, 120000);
    return () => clearInterval(timer);
  }, []);

  const filteredNews = filter === 'all'
    ? news
    : news.filter(n => n.tags.includes(filter));

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
        <Space>
          <Title level={4} style={{ margin: 0 }}>新闻资讯</Title>
          <Badge count={news.length} style={{ backgroundColor: '#1677ff' }} />
        </Space>
        <Space>
          {lastUpdate && <Text type="secondary" style={{ fontSize: 12 }}>更新: {lastUpdate}</Text>}
          <Select
            size="small"
            value={filter}
            style={{ width: 100 }}
            onChange={setFilter}
            options={[
              { value: 'all', label: '全部' },
              { value: 'policy', label: '政策' },
              { value: 'macro', label: '宏观' },
              { value: 'earnings', label: '财报' },
              { value: 'industry', label: '行业' },
              { value: 'market', label: '市场' },
            ]}
          />
          <Button size="small" icon={<ReloadOutlined />} onClick={fetchNews} loading={loading}>刷新</Button>
        </Space>
      </div>

      {loading && news.length === 0 ? (
        <div style={{ display: 'flex', justifyContent: 'center', minHeight: 300, alignItems: 'center' }}>
          <Spin size="large" />
        </div>
      ) : filteredNews.length === 0 ? (
        <Empty description="暂无新闻数据" />
      ) : (
        <List
          dataSource={filteredNews}
          renderItem={(item) => (
            <Card
              size="small"
              style={{ marginBottom: 8 }}
              hoverable
            >
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start' }}>
                <div style={{ flex: 1 }}>
                  <Text strong style={{ fontSize: 15 }}>{item.title}</Text>
                  <Paragraph
                    type="secondary"
                    style={{ marginTop: 6, marginBottom: 8, fontSize: 13 }}
                    ellipsis={{ rows: 2 }}
                  >
                    {item.content}
                  </Paragraph>
                  <Space size={[8, 4]} wrap>
                    <Text type="secondary" style={{ fontSize: 12 }}>
                      <ClockCircleOutlined /> {new Date(item.published_at).toLocaleString('zh-CN')}
                    </Text>
                    <Text type="secondary" style={{ fontSize: 12 }}>
                      {sourceLabel[item.source] || item.source}
                    </Text>
                    {item.tags.map(tag => (
                      <Tag key={tag} color={tagColor[tag] || 'default'} style={{ fontSize: 11 }}>
                        {tagLabel[tag] || tag}
                      </Tag>
                    ))}
                    {item.symbols.map(sym => (
                      <Tag key={sym} icon={<TagOutlined />} style={{ fontSize: 11 }}>{sym}</Tag>
                    ))}
                  </Space>
                </div>
              </div>
            </Card>
          )}
        />
      )}
    </div>
  );
};

export default News;