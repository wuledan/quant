import React, { useEffect, useRef, useState, useCallback } from 'react';
import dayjs from 'dayjs';
import { Card, Col, Row, Select, Spin, Typography, Statistic, Empty, Space, DatePicker, Button, message, Input } from 'antd';
import { SearchOutlined, ReloadOutlined } from '@ant-design/icons';
import { createChart, CandlestickSeries } from 'lightweight-charts';
import type { IChartApi, Time } from 'lightweight-charts';

const { Title, Text } = Typography;
const { RangePicker } = DatePicker;
const API_BASE = '/api/v1';

interface KlinePoint {
  date: string;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  amount: number;
}

const popularSymbols = [
  { label: '沪深300', value: '000300.SH' },
  { label: '上证指数', value: '000001.SH' },
  { label: '深证成指', value: '399001.SZ' },
  { label: '创业板指', value: '399006.SZ' },
  { label: '贵州茅台', value: '600519.SH' },
  { label: '宁德时代', value: '300750.SZ' },
  { label: '中国平安', value: '601318.SH' },
  { label: '招商银行', value: '600036.SH' },
  { label: '比亚迪', value: '002594.SZ' },
  { label: '中芯国际', value: '688981.SH' },
];

const MarketData: React.FC = () => {
  const [symbol, setSymbol] = useState<string>('000300.SH');
  const [searchText, setSearchText] = useState<string>('');
  const [loading, setLoading] = useState(false);
  const [data, setData] = useState<KlinePoint[]>([]);
  const [dateRange, setDateRange] = useState<[string, string]>(['2024-01-01', '']);
  const [lastUpdate, setLastUpdate] = useState<string>('');

  const chartContainerRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<IChartApi | null>(null);

  const fetchData = useCallback(async (sym: string, range?: [string, string]) => {
    setLoading(true);
    try {
      const r = range || dateRange;
      const params = new URLSearchParams();
      params.set('start_date', r[0] || '2024-01-01');
      params.set('end_date', r[1] || '');
      const res = await fetch(`${API_BASE}/data/daily/${sym}?${params}`);
      if (!res.ok) {
        message.error('获取行情数据失败');
        setData([]);
        return;
      }
      const json = await res.json();
      // Backend returns plain array, not {data: [...]}
      const arr: Record<string, unknown>[] = Array.isArray(json) ? json : (json.data || []);
      const raw: KlinePoint[] = arr.map((d: Record<string, unknown>) => ({
        date: typeof d.timestamp === 'number'
          ? dayjs(d.timestamp as number).format('YYYY-MM-DD')
          : (d.date as string || ''),
        open: d.open as number,
        high: d.high as number,
        low: d.low as number,
        close: d.close as number,
        volume: d.volume as number,
        amount: d.amount as number,
      }));
      setData(raw);
      setLastUpdate(new Date().toLocaleTimeString('zh-CN'));
    } catch {
      message.error('网络错误');
      setData([]);
    } finally {
      setLoading(false);
    }
  }, [dateRange]);

  const handleSearch = () => {
    const trimmed = searchText.trim().toUpperCase();
    if (!trimmed) return;
    // 支持 "600519" "600519.SH" 格式
    let sym = trimmed;
    if (/^\d{6}$/.test(trimmed)) {
      sym = trimmed.startsWith('6') || trimmed.startsWith('5')
        ? `${trimmed}.SH`
        : `${trimmed}.SZ`;
    }
    setSymbol(sym);
    fetchData(sym);
  };

  const handleDateRangeChange = (dates: [dayjs.Dayjs | null, dayjs.Dayjs | null] | null) => {
    if (dates && dates[0] && dates[1]) {
      const range: [string, string] = [dates[0].format('YYYY-MM-DD'), dates[1].format('YYYY-MM-DD')];
      setDateRange(range);
      fetchData(symbol, range);
    }
  };

  useEffect(() => {
    fetchData(symbol);
    const timer = setInterval(() => fetchData(symbol), 120000);
    return () => clearInterval(timer);
  }, [symbol]);

  // K-line chart
  useEffect(() => {
    if (!chartContainerRef.current) return;

    if (chartRef.current) {
      chartRef.current.remove();
      chartRef.current = null;
    }

    if (data.length === 0) return;

    const chart = createChart(chartContainerRef.current, {
      layout: { background: { color: 'transparent' }, textColor: '#333' },
      grid: { vertLines: { color: '#e8e8e8' }, horzLines: { color: '#e8e8e8' } },
      width: chartContainerRef.current.clientWidth,
      height: 500,
      rightPriceScale: { borderColor: '#d9d9d9' },
      timeScale: { borderColor: '#d9d9d9', timeVisible: false },
      crosshair: {
        horzLine: { color: '#999', labelBackgroundColor: '#1677ff' },
      },
      handleScroll: { vertTouchDrag: false },
    });

    const candlestickSeries = chart.addSeries(CandlestickSeries, {
      upColor: '#f5222d',
      downColor: '#52c41a',
      borderUpColor: '#f5222d',
      borderDownColor: '#52c41a',
      wickUpColor: '#f5222d',
      wickDownColor: '#52c41a',
    });

    candlestickSeries.setData(
      data.map(p => ({
        time: p.date as Time,
        open: p.open,
        high: p.high,
        low: p.low,
        close: p.close,
      }))
    );

    chart.timeScale().fitContent();
    chartRef.current = chart;

    const handleResize = () => {
      if (chartContainerRef.current) {
        chart.applyOptions({ width: chartContainerRef.current.clientWidth });
      }
    };
    window.addEventListener('resize', handleResize);

    return () => {
      window.removeEventListener('resize', handleResize);
      chart.remove();
      chartRef.current = null;
    };
  }, [data]);

  const latest = data.length > 0 ? data[data.length - 1] : null;
  const first = data.length > 0 ? data[0] : null;
  const change = latest && data.length > 1
    ? ((latest.close - data[data.length - 2].close) / data[data.length - 2].close * 100)
    : 0;
  const rangeChange = latest && first && first.close > 0
    ? ((latest.close - first.close) / first.close * 100)
    : 0;

  const symbolLabel = popularSymbols.find(s => s.value === symbol)?.label || symbol;

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <Title level={4} style={{ margin: 0 }}>行情数据</Title>
        {lastUpdate && <Text type="secondary" style={{ fontSize: 12 }}>最后更新: {lastUpdate}</Text>}
      </div>

      <Card style={{ marginBottom: 16 }}>
        <Space wrap size="middle">
          <Select
            showSearch
            style={{ width: 200 }}
            value={symbol}
            onChange={(v) => { setSymbol(v); fetchData(v); }}
            options={popularSymbols}
            placeholder="选择标的"
            filterOption={(input, option) => (option?.label ?? '').includes(input) || (option?.value ?? '').includes(input)}
          />
          <Input.Search
            placeholder="输入股票代码 如 600519"
            style={{ width: 220 }}
            value={searchText}
            onChange={(e) => setSearchText(e.target.value)}
            onSearch={handleSearch}
            enterButton={<SearchOutlined />}
          />
          <RangePicker
            placeholder={['开始日期', '结束日期']}
            onChange={handleDateRangeChange as any}
          />
          <Button icon={<ReloadOutlined />} onClick={() => fetchData(symbol)}>刷新</Button>
        </Space>
      </Card>

      {loading ? (
        <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
          <Spin size="large" />
        </div>
      ) : data.length === 0 ? (
        <Empty description="暂无行情数据" />
      ) : (
        <>
          <Row gutter={[12, 12]} style={{ marginBottom: 16 }}>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic title={`${symbolLabel} 最新价`} value={latest?.close ?? '-'} precision={2} prefix="¥" />
              </Card>
            </Col>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic
                  title="日涨跌幅"
                  value={change}
                  precision={2}
                  suffix="%"
                  valueStyle={{ color: change >= 0 ? '#f5222d' : '#52c41a' }}
                  prefix={change >= 0 ? '▲' : '▼'}
                />
              </Card>
            </Col>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic
                  title="区间涨跌幅"
                  value={rangeChange}
                  precision={2}
                  suffix="%"
                  valueStyle={{ color: rangeChange >= 0 ? '#f5222d' : '#52c41a' }}
                />
              </Card>
            </Col>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic title="最高" value={Math.max(...data.map(d => d.high))} precision={2} prefix="¥" />
              </Card>
            </Col>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic title="最低" value={Math.min(...data.map(d => d.low))} precision={2} prefix="¥" />
              </Card>
            </Col>
            <Col xs={12} sm={8} md={6} lg={4}>
              <Card size="small">
                <Statistic title="数据条数" value={data.length} suffix="条" />
              </Card>
            </Col>
          </Row>

          <Card title={`${symbolLabel} (${symbol}) K线图`} styles={{ body: { padding: 0 } }}>
            <div ref={chartContainerRef} style={{ width: '100%' }} />
          </Card>
        </>
      )}
    </div>
  );
};

export default MarketData;