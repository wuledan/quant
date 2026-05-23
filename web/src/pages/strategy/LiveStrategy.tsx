import React, { useEffect, useRef, useState, useCallback } from 'react';
import { useParams } from 'react-router-dom';
import {
  Card, Button, Tag, Typography, Spin, Alert, Empty, Space, Table, Statistic, Row, Col, message, Descriptions,
} from 'antd';
import {
  PlayCircleOutlined, PauseCircleOutlined, ReloadOutlined,
} from '@ant-design/icons';
import { createChart, CandlestickSeries, LineSeries } from 'lightweight-charts';
import type { IChartApi, Time } from 'lightweight-charts';
import { MarketWebSocket } from '../../api/websocket';
import type { WsEnvelope } from '../../api/websocket';

const { Title, Text } = Typography;
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
  volume: number;
  amount: number;
}

interface LiveSignal {
  type: 'buy' | 'sell' | 'hold';
  quantity: number;
  price: number;
  confidence: number;
}

interface LiveStatus {
  strategy_name: string;
  symbol: string;
  status: 'running' | 'paused' | 'stopped';
  signal?: LiveSignal;
  factors?: Record<string, number>;
  factors_history?: Record<string, number[]>;
}

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

const statusColorMap: Record<string, string> = {
  running: 'success', paused: 'warning', stopped: 'default',
};
const statusLabelMap: Record<string, string> = {
  running: '运行中', paused: '已暂停', stopped: '已停止',
};

const factorColors: Record<string, string> = {
  SMA_5: '#1677ff', SMA_10: '#fa8c16', SMA_20: '#722ed1', SMA_60: '#eb2f96',
};

const signalColorMap: Record<string, string> = {
  buy: 'success', sell: 'error', hold: 'default',
};
const signalLabelMap: Record<string, string> = {
  buy: '买入', sell: '卖出', hold: '持有',
};

/* ------------------------------------------------------------------ */
/*  Component                                                          */
/* ------------------------------------------------------------------ */

const LiveStrategy: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  const [status, setStatus] = useState<LiveStatus | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [klineData, setKlineData] = useState<KlinePoint[]>([]);
  const [liveSignal, setLiveSignal] = useState<LiveSignal | null>(null);
  const [factorValues, setFactorValues] = useState<Record<string, number>>({});
  const [factorHistory, setFactorHistory] = useState<Record<string, number[]>>({});
  const [wsConnected, setWsConnected] = useState(false);

  const chartContainerRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<IChartApi | null>(null);
  const wsRef = useRef<MarketWebSocket | null>(null);

  /* ---- REST: fetch live status ---- */

  const fetchStatus = useCallback(async () => {
    if (!id) return;
    try {
      const res = await fetch(`${API_BASE}/strategies/${id}/live_status`);
      if (!res.ok) throw new Error('获取状态失败');
      const data: LiveStatus = await res.json();
      setStatus(data);
      setFactorValues(data.factors || {});
      setFactorHistory(data.factors_history || {});
      if (data.signal) setLiveSignal(data.signal);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e.message : '未知错误');
    } finally {
      setLoading(false);
    }
  }, [id]);

  /* ---- REST: fetch kline for chart background ---- */

  const fetchKline = useCallback(async (symbol: string) => {
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
        volume: d.volume as number,
        amount: d.amount as number,
      }));
      setKlineData(raw);
    } catch {
      // silent - kline is optional background
    }
  }, []);

  /* ---- controls ---- */

  const handleStartLive = async () => {
    if (!id) return;
    try {
      const res = await fetch(`${API_BASE}/strategies/${id}/start_live`, { method: 'POST' });
      if (!res.ok) throw new Error('启动失败');
      message.success('已启动实盘');
      fetchStatus();
    } catch {
      message.error('启动实盘失败');
    }
  };

  const handleStopLive = async () => {
    if (!id) return;
    try {
      const res = await fetch(`${API_BASE}/strategies/${id}/stop_live`, { method: 'POST' });
      if (!res.ok) throw new Error('停止失败');
      message.success('已停止实盘');
      fetchStatus();
    } catch {
      message.error('停止实盘失败');
    }
  };

  /* ---- effects ---- */

  // Poll live status
  useEffect(() => {
    fetchStatus();
    const timer = setInterval(fetchStatus, 10000);
    return () => clearInterval(timer);
  }, [fetchStatus]);

  // Fetch kline when symbol becomes available
  useEffect(() => {
    if (status?.symbol) {
      fetchKline(status.symbol);
    }
  }, [status?.symbol, fetchKline]);

  // WebSocket for real-time signal + factor push
  useEffect(() => {
    const ws = new MarketWebSocket();
    wsRef.current = ws;

    const unsubOpen = ws.onOpen(() => setWsConnected(true));
    const unsubClose = ws.onClose(() => setWsConnected(false));
    const unsubMessage = ws.onMessage((envelope: WsEnvelope) => {
      if (envelope.channel === 'signal') {
        const signalData = envelope.data as LiveSignal;
        setLiveSignal(signalData);
      }
      if (envelope.channel === 'factor') {
        const factorData = envelope.data as Record<string, number>;
        if (factorData) setFactorValues(prev => ({ ...prev, ...factorData }));
      }
    });

    ws.connect();
    ws.subscribe('signal');
    ws.subscribe('factor');

    return () => {
      unsubOpen();
      unsubClose();
      unsubMessage();
      ws.disconnect();
      wsRef.current = null;
    };
  }, []);

  /* ---- kline + marker chart ---- */

  useEffect(() => {
    if (!chartContainerRef.current || klineData.length === 0) return;

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

    // Factor overlay lines
    Object.entries(factorHistory).forEach(([name, values]) => {
      if (values.length > 0) {
        const lineSeries = chart.addSeries(LineSeries, {
          color: factorColors[name] || '#52c41a',
          lineWidth: 1,
          priceLineVisible: false,
          lastValueVisible: false,
        });
        const lineData = values.map((v, i) => ({
          time: klineData[i]?.date as Time,
          value: v > 0 ? v : (klineData[i]?.close || 0),
        })).filter(p => p.time);
        lineSeries.setData(lineData);
      }
    });

    // Buy/sell markers from live signal
    if (liveSignal && (liveSignal.type === 'buy' || liveSignal.type === 'sell') && klineData.length > 0) {
      const lastKline = klineData[klineData.length - 1];
      const isBuy = liveSignal.type === 'buy';
      candlestickSeries.setMarkers([{
        time: lastKline.date as Time,
        position: isBuy ? 'belowBar' : 'aboveBar',
        color: isBuy ? '#52c41a' : '#f5222d',
        shape: isBuy ? 'arrowUp' : 'arrowDown',
        text: `${isBuy ? 'BUY' : 'SELL'} ${liveSignal.quantity}@${liveSignal.price}`,
      }]);
    }

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
  }, [klineData, factorHistory, liveSignal]);

  /* ---- derived data ---- */

  const factorTableData = Object.entries(factorValues).map(([k, v]) => ({ key: k, name: k, value: v }));

  const signalColor = liveSignal ? signalColorMap[liveSignal.type] || 'default' : 'default';
  const signalLabel = liveSignal ? signalLabelMap[liveSignal.type] || liveSignal.type : '-';

  /* ---- render ---- */

  if (loading && !status) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
        <Spin size="large" />
      </div>
    );
  }

  if (error && !status) {
    return (
      <Alert
        message="加载失败"
        description={error}
        type="error"
        showIcon
        action={<Button onClick={fetchStatus}>重试</Button>}
      />
    );
  }

  if (!status) {
    return <Empty description="未找到策略运行状态" />;
  }

  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <Title level={4} style={{ margin: 0 }}>
          {status.strategy_name || `策略 #${id}`}
          <Tag color={statusColorMap[status.status]} style={{ marginLeft: 8 }}>
            {statusLabelMap[status.status]}
          </Tag>
          {wsConnected
            ? <Tag color="processing">WebSocket 已连接</Tag>
            : <Tag>WebSocket 未连接</Tag>}
        </Title>
        <Space>
          <Button icon={<ReloadOutlined />} onClick={fetchStatus}>刷新</Button>
          {status.status === 'running' ? (
            <Button icon={<PauseCircleOutlined />} onClick={handleStopLive} danger>停止实盘</Button>
          ) : (
            <Button type="primary" icon={<PlayCircleOutlined />} onClick={handleStartLive}>启动实盘</Button>
          )}
        </Space>
      </div>

      <Row gutter={[12, 12]} style={{ marginBottom: 16 }}>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic title="标的" value={status.symbol || '-'} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic
              title="最新信号"
              valueRender={() => (
                <Tag color={signalColor} style={{ fontSize: 14, padding: '2px 8px' }}>{signalLabel}</Tag>
              )}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic title="建议数量" value={liveSignal?.quantity ?? '-'} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic
              title="参考价格"
              value={liveSignal?.price ?? '-'}
              precision={2}
              prefix="¥"
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic
              title="置信度"
              value={liveSignal?.confidence != null ? (liveSignal.confidence * 100).toFixed(1) + '%' : '-'}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={4}>
          <Card size="small">
            <Statistic title="K线数据" value={klineData.length} suffix="条" />
          </Card>
        </Col>
      </Row>

      <Row gutter={12}>
        <Col xs={24} lg={16}>
          <Card title="K线图" styles={{ body: { padding: 0 } }}>
            <div ref={chartContainerRef} style={{ width: '100%' }} />
          </Card>
        </Col>
        <Col xs={24} lg={8}>
          <Card title="实时因子值" size="small" style={{ marginBottom: 12 }}>
            <Table
              dataSource={factorTableData}
              columns={[
                { title: '因子名', dataIndex: 'name', key: 'name' },
                {
                  title: '当前值', dataIndex: 'value', key: 'value',
                  render: (v: number) => v != null ? v.toFixed(4) : '-',
                },
              ]}
              rowKey="key"
              pagination={false}
              size="small"
              locale={{ emptyText: '暂无因子数据' }}
            />
          </Card>
          <Card title="最新信号详情" size="small">
            {liveSignal ? (
              <Descriptions column={1} size="small">
                <Descriptions.Item label="信号类型">
                  <Tag color={signalColor}>{signalLabel}</Tag>
                </Descriptions.Item>
                <Descriptions.Item label="数量">{liveSignal.quantity}</Descriptions.Item>
                <Descriptions.Item label="价格">¥{liveSignal.price?.toFixed(2)}</Descriptions.Item>
                <Descriptions.Item label="置信度">{(liveSignal.confidence * 100).toFixed(1)}%</Descriptions.Item>
              </Descriptions>
            ) : (
              <Text type="secondary">暂无信号</Text>
            )}
          </Card>
        </Col>
      </Row>
    </div>
  );
};

export default LiveStrategy;
