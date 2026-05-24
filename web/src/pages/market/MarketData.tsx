import React, { useEffect, useRef, useState, useCallback } from 'react';
import dayjs from 'dayjs';
import { Card, Col, Row, Tag, Spin, Typography, Statistic, Empty, Space, DatePicker, Button, message, Input } from 'antd';
import { SearchOutlined, ReloadOutlined } from '@ant-design/icons';
import { createChart, CandlestickSeries, LineSeries } from 'lightweight-charts';
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

interface SymbolInfo {
  symbol: string;
  name: string;
}

const MarketData: React.FC = () => {
  const [symbol, setSymbol] = useState<string>('000300.SH');
  const [searchText, setSearchText] = useState<string>('');
  const [loading, setLoading] = useState(false);
  const [data, setData] = useState<KlinePoint[]>([]);
  const [factors, setFactors] = useState<Record<string, number[]>>({});
  const [showFactors, setShowFactors] = useState<Record<string, boolean>>({ SMA_5: true, SMA_10: true, SMA_20: true, SMA_60: false });
  const [dateRange, setDateRange] = useState<[string, string]>(['2024-01-01', '']);
  const [lastUpdate, setLastUpdate] = useState<string>('');
  const [allSymbols, setAllSymbols] = useState<SymbolInfo[]>([]);

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

      // Fetch factors
      try {
        const fr = await fetch(`${API_BASE}/factors/compute?symbol=${sym}&${params}`, { method: 'POST' });
        if (fr.ok) {
          const fj = await fr.json();
          setFactors(fj.factors || {});
        }
      } catch { /* factors optional */ }
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

  // Fetch available symbols
  useEffect(() => {
    fetch(`${API_BASE}/symbols`)
      .then(r => r.json())
      .then(d => { if (Array.isArray(d)) setAllSymbols(d); })
      .catch(() => {});
  }, []);

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
      layout: { background: { color: '#ffffff' }, textColor: '#333' },
      grid: { vertLines: { color: '#f0f0f0' }, horzLines: { color: '#f0f0f0' } },
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

    // Add factor overlay lines (visible when showFactors[key] is true)
    const factorColors: Record<string, {color: string; width: number}> = {
      SMA_5:  { color: '#FF6B35', width: 2 },
      SMA_10: { color: '#00C9A7', width: 2 },
      SMA_20: { color: '#C77DFF', width: 2 },
      SMA_60: { color: '#FFD166', width: 2 },
    };
    Object.entries(factors).forEach(([name, values]) => {
      if (showFactors[name] && values.length > 0) {
        const fc = factorColors[name];
        const lineSeries = chart.addSeries(LineSeries, {
          color: fc?.color || '#52c41a',
          lineWidth: fc?.width || 2,
          priceLineVisible: false,
          lastValueVisible: false,
        });
        const lineData = values.map((v, i) => ({
          time: data[i]?.date as Time,
          value: v > 0 ? v : (data[i]?.close || 0),
        })).filter(p => p.time);
        lineSeries.setData(lineData);
      }
    });

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
  }, [data, factors, showFactors]);

  const latest = data.length > 0 ? data[data.length - 1] : null;
  const first = data.length > 0 ? data[0] : null;
  const change = latest && data.length > 1
    ? ((latest.close - data[data.length - 2].close) / data[data.length - 2].close * 100)
    : 0;
  const rangeChange = latest && first && first.close > 0
    ? ((latest.close - first.close) / first.close * 100)
    : 0;

  const hotTags = allSymbols.slice(0, 8);
  const symbolLabel = allSymbols.find(s => s.symbol === symbol)?.name || symbol;

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <Title level={4} style={{ margin: 0 }}>行情数据</Title>
        {lastUpdate && <Text type="secondary" style={{ fontSize: 12 }}>最后更新: {lastUpdate}</Text>}
      </div>

      <Card style={{ marginBottom: 16 }}>
        <Space wrap size="middle">
          <Input.Search
            placeholder="输入股票代码 如 000001.SZ"
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
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginTop: 12 }}>
          {hotTags.map(s => (
            <Tag key={s.symbol} style={{ cursor: 'pointer' }}
              onClick={() => { setSymbol(s.symbol); fetchData(s.symbol); }}>
              {s.symbol}
            </Tag>
          ))}
        </div>
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

          <div style={{ marginBottom: 8, display: 'flex', gap: 8, flexWrap: 'wrap' }}>
            {Object.keys(factors).length > 0 && (
              <Space size="small">
                {Object.entries(showFactors).map(([k, v]) => (
                  <Button key={k} size="small" type={v ? 'primary' : 'default'}
                    onClick={() => setShowFactors(s => ({ ...s, [k]: !s[k] }))}>
                    {k}
                  </Button>
                ))}
              </Space>
            )}
          </div>
          <Card title={`${symbolLabel} (${symbol}) K线图`} styles={{ body: { padding: 0 } }}>
            <div ref={chartContainerRef} style={{ width: '100%' }} />
          </Card>
        </>
      )}
    </div>
  );
};

export default MarketData;