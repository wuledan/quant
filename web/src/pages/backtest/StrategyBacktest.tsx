import React, { useEffect, useRef, useState, useCallback } from 'react';
import { useParams } from 'react-router-dom';
import {
  Card, Button, Form, Select, InputNumber, DatePicker, Typography, Spin, Alert, Empty,
  Space, Table, Statistic, Row, Col, message, Tag, Divider,
} from 'antd';
import { PlayCircleOutlined } from '@ant-design/icons';
import { createChart, CandlestickSeries, LineSeries } from 'lightweight-charts';
import type { IChartApi, Time } from 'lightweight-charts';
import dayjs from 'dayjs';

const { Title, Text } = Typography;
const { RangePicker } = DatePicker;
const API_BASE = '/api/v1';

/* ------------------------------------------------------------------ */
/*  Types                                                              */
/* ------------------------------------------------------------------ */

interface KlinePoint {
  date: string;
  open: number;
  high: number;
  low: number;
  close: number;
}

interface BacktestMetrics {
  total_return: number;
  annual_return: number;
  max_drawdown: number;
  sharpe_ratio: number;
  total_trades: number;
  win_rate: number;
  profit_factor: number;
}

interface NavPoint {
  date: string;
  nav: number;
}

interface TradeRecord {
  date: string;
  direction: string;
  quantity: number;
  price: number;
}

interface BacktestResponse {
  metrics: BacktestMetrics;
  nav_curve: NavPoint[];
  trades: TradeRecord[];
}

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

const popularSymbols = [
  { label: '沪深300', value: '000300.SH' },
  { label: '上证指数', value: '000001.SH' },
  { label: '深证成指', value: '399001.SZ' },
  { label: '创业板指', value: '399006.SZ' },
  { label: '贵州茅台', value: '600519.SH' },
  { label: '宁德时代', value: '300750.SZ' },
];

const tradeColumns = [
  { title: '日期', dataIndex: 'date', key: 'date', width: 120 },
  {
    title: '方向', dataIndex: 'direction', key: 'direction', width: 80,
    render: (dir: string) => (
      <Tag color={dir.toUpperCase() === 'BUY' ? 'red' : 'green'}>
        {dir.toUpperCase() === 'BUY' ? '买入' : '卖出'}
      </Tag>
    ),
  },
  {
    title: '数量', dataIndex: 'quantity', key: 'quantity', width: 100, align: 'right' as const,
    render: (v: number) => v?.toLocaleString() ?? '-',
  },
  {
    title: '成交价', dataIndex: 'price', key: 'price', width: 120, align: 'right' as const,
    render: (v: number) => v != null ? `¥${v.toFixed(2)}` : '-',
  },
];

/* ------------------------------------------------------------------ */
/*  Component                                                          */
/* ------------------------------------------------------------------ */

const StrategyBacktest: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  const [form] = Form.useForm();
  const [loading, setLoading] = useState(false);
  const [fetchingKline, setFetchingKline] = useState(false);
  const [result, setResult] = useState<BacktestResponse | null>(null);
  const [klineData, setKlineData] = useState<KlinePoint[]>([]);
  const [submitted, setSubmitted] = useState(false);
  const [fetchError, setFetchError] = useState<string | null>(null);

  const chartContainerRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<IChartApi | null>(null);

  /* ---- fetch kline for chart background ---- */

  const fetchKline = useCallback(async (symbol: string) => {
    setFetchingKline(true);
    try {
      const res = await fetch(`${API_BASE}/data/daily/${symbol}`);
      if (!res.ok) return;
      const json = await res.json();
      const arr: Record<string, unknown>[] = Array.isArray(json) ? json : (json.data || []);
      const raw: KlinePoint[] = arr.map((d: Record<string, unknown>) => ({
        date: (d.date as string) || '',
        open: d.open as number,
        high: d.high as number,
        low: d.low as number,
        close: d.close as number,
      }));
      setKlineData(raw);
    } catch {
      // silent
    } finally {
      setFetchingKline(false);
    }
  }, []);

  /* ---- submit backtest ---- */

  const handleSubmit = async (values: Record<string, unknown>) => {
    if (!id) return;

    const dateRange = values.dateRange as unknown as [dayjs.Dayjs, dayjs.Dayjs];
    if (!dateRange || dateRange.length < 2) {
      message.error('请选择回测日期范围');
      return;
    }

    const symbol = values.symbol as string;
    if (!symbol) {
      message.error('请选择标的');
      return;
    }

    setLoading(true);
    setSubmitted(true);
    setFetchError(null);

    try {
      const res = await fetch(`${API_BASE}/strategies/${id}/backtest`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          symbols: [symbol],
          start_date: dateRange[0].format('YYYY-MM-DD'),
          end_date: dateRange[1].format('YYYY-MM-DD'),
          initial_cash: values.initial_capital || 1000000,
        }),
      });
      if (!res.ok) {
        const errData = await res.json().catch(() => ({ detail: '请求失败' }));
        throw new Error(errData.detail || '回测失败');
      }
      const data: BacktestResponse = await res.json();
      setResult(data);
      message.success('回测完成');
      // Fetch kline for chart background
      fetchKline(symbol);
    } catch (e) {
      const msg = e instanceof Error ? e.message : '网络错误';
      setFetchError(msg);
      message.error(msg);
    } finally {
      setLoading(false);
    }
  };

  /* ---- chart: NAV curve + kline + trade markers ---- */

  useEffect(() => {
    if (!chartContainerRef.current || klineData.length === 0 || !result) return;

    if (chartRef.current) {
      chartRef.current.remove();
      chartRef.current = null;
    }

    const chart = createChart(chartContainerRef.current, {
      layout: { background: { color: 'transparent' }, textColor: '#333' },
      grid: { vertLines: { color: '#e8e8e8' }, horzLines: { color: '#e8e8e8' } },
      width: chartContainerRef.current.clientWidth,
      height: 500,
      rightPriceScale: { borderColor: '#d9d9d9' },
      timeScale: { borderColor: '#d9d9d9', timeVisible: false },
      crosshair: { horzLine: { color: '#999', labelBackgroundColor: '#1677ff' } },
      handleScroll: { vertTouchDrag: false },
    });

    const candlestickSeries = chart.addSeries(CandlestickSeries, {
      upColor: '#f5222d', downColor: '#52c41a',
      borderUpColor: '#f5222d', borderDownColor: '#52c41a',
      wickUpColor: '#f5222d', wickDownColor: '#52c41a',
    });

    candlestickSeries.setData(
      klineData.map(p => ({
        time: p.date as Time,
        open: p.open, high: p.high, low: p.low, close: p.close,
      }))
    );

    // Trade markers on candlestick series
    if (result.trades && result.trades.length > 0) {
      candlestickSeries.setMarkers(
        result.trades.map(t => ({
          time: t.date as Time,
          position: t.direction.toUpperCase() === 'BUY' ? 'belowBar' as const : 'aboveBar' as const,
          color: t.direction.toUpperCase() === 'BUY' ? '#52c41a' : '#f5222d',
          shape: t.direction.toUpperCase() === 'BUY' ? 'arrowUp' as const : 'arrowDown' as const,
          text: `${t.direction.toUpperCase() === 'BUY' ? 'B' : 'S'} ${t.quantity}@${t.price}`,
        }))
      );
    }

    // NAV curve overlay
    const navSeries = chart.addSeries(LineSeries, {
      color: '#1677ff',
      lineWidth: 2,
      priceLineVisible: false,
      lastValueVisible: true,
    });

    navSeries.setData(
      result.nav_curve.map(p => ({
        time: p.date as Time,
        value: p.nav,
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
  }, [klineData, result]);

  /* ---- render ---- */

  const profitColor = '#52c41a';
  const lossColor = '#f5222d';

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <Title level={4} style={{ margin: 0 }}>策略回测 #{id}</Title>
      </div>

      <Card title="回测参数" style={{ marginBottom: 16 }}>
        <Form
          form={form}
          layout="inline"
          onFinish={handleSubmit}
          initialValues={{ initial_capital: 1000000, symbol: undefined, dateRange: [dayjs().subtract(1, 'month'), dayjs()] }}
          style={{ flexWrap: 'wrap', gap: 8 }}
        >
          <Form.Item name="symbol" label="标的" rules={[{ required: true, message: '请选择标的' }]}>
            <Select
              style={{ width: 200 }}
              placeholder="选择标的"
              showSearch
              optionFilterProp="label"
              options={popularSymbols}
            />
          </Form.Item>
          <Form.Item name="dateRange" label="回测区间" rules={[{ required: true, message: '请选择日期范围' }]}>
            <RangePicker />
          </Form.Item>
          <Form.Item name="initial_capital" label="初始资金">
            <InputNumber
              min={100000}
              max={100000000}
              step={100000}
              style={{ width: 170 }}
              addonAfter="¥"
            />
          </Form.Item>
          <Form.Item>
            <Button type="primary" htmlType="submit" icon={<PlayCircleOutlined />} loading={loading}>
              开始回测
            </Button>
          </Form.Item>
        </Form>
      </Card>

      {/* Loading state */}
      {loading && (
        <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 200 }}>
          <Spin size="large" tip="回测运行中...">
            <div style={{ padding: 50 }} />
          </Spin>
        </div>
      )}

      {/* Error state */}
      {fetchError && !loading && (
        <Alert
          message="回测失败"
          description={fetchError}
          type="error"
          showIcon
          style={{ marginBottom: 16 }}
          closable
        />
      )}

      {/* Results */}
      {result && !loading && (
        <>
          <Title level={5}>绩效指标</Title>
          <Row gutter={[12, 12]} style={{ marginBottom: 16 }}>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic
                  title="总收益率"
                  value={(result.metrics.total_return * 100)}
                  suffix="%"
                  precision={2}
                  valueStyle={{ color: result.metrics.total_return >= 0 ? profitColor : lossColor }}
                />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic
                  title="年化收益"
                  value={(result.metrics.annual_return * 100)}
                  suffix="%"
                  precision={2}
                  valueStyle={{ color: result.metrics.annual_return >= 0 ? profitColor : lossColor }}
                />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic title="夏普比率" value={result.metrics.sharpe_ratio} precision={2} />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic
                  title="最大回撤"
                  value={Math.abs(result.metrics.max_drawdown * 100)}
                  suffix="%"
                  precision={2}
                  valueStyle={{ color: lossColor }}
                />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic title="胜率" value={result.metrics.win_rate * 100} suffix="%" precision={1} />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic title="盈亏比" value={result.metrics.profit_factor} precision={2} />
              </Card>
            </Col>
            <Col xs={12} lg={6}>
              <Card size="small">
                <Statistic title="交易次数" value={result.metrics.total_trades} />
              </Card>
            </Col>
          </Row>

          <Title level={5}>净值曲线 & K线</Title>
          <Card style={{ marginBottom: 16 }} styles={{ body: { padding: 0 } }}>
            {fetchingKline ? (
              <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
                <Spin />
              </div>
            ) : (
              <div ref={chartContainerRef} style={{ width: '100%' }} />
            )}
          </Card>

          {result.trades && result.trades.length > 0 && (
            <>
              <Divider orientation="left" plain>交易记录</Divider>
              <Table
                dataSource={result.trades}
                columns={tradeColumns}
                rowKey={(_, idx) => `${idx}`}
                pagination={{ pageSize: 10, showSizeChanger: true, pageSizeOptions: ['5', '10', '20', '50'], showTotal: (total) => `共 ${total} 条` }}
                size="small"
                bordered
                locale={{ emptyText: '暂无交易记录' }}
              />
            </>
          )}
        </>
      )}

      {/* Submitted but empty */}
      {!result && !loading && submitted && !fetchError && (
        <Empty description="回测未返回有效数据" />
      )}

      {/* Initial state */}
      {!submitted && !loading && (
        <div style={{ textAlign: 'center', padding: 40 }}>
          <Text type="secondary">请填写回测参数并点击「开始回测」</Text>
        </div>
      )}
    </div>
  );
};

export default StrategyBacktest;
