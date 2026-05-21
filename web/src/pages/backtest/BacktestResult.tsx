import { useEffect, useRef, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import {
  Card, Col, Row, Statistic, Table, Tag, Descriptions, Divider,
  Typography, Spin, Alert, Empty, Button, theme,
} from 'antd';
import { ArrowLeftOutlined } from '@ant-design/icons';
import { createChart, LineSeries, AreaSeries } from 'lightweight-charts';
import type { Time } from 'lightweight-charts';

const { Title, Text } = Typography;
const API_BASE = '/api/v1';

/* ------------------------------------------------------------------ */
/*  API response types                                                 */
/* ------------------------------------------------------------------ */

interface BacktestMetrics {
  total_return: number;
  annual_return: number;
  max_drawdown: number;
  sharpe_ratio: number;
  win_rate: number;
  total_trades: number;
  profit_factor: number;
}

interface NavPoint {
  date: string;
  nav: number;
}

interface TradeRecord {
  date: string;
  symbol: string;
  direction: string;
  quantity: number;
  price: number;
}

/* ------------------------------------------------------------------ */
/*  Drawdown computation                                               */
/* ------------------------------------------------------------------ */

function computeDrawdown(
  navPoints: NavPoint[],
): { time: string; value: number }[] {
  let peak = -Infinity;
  return navPoints.map((p) => {
    if (p.nav > peak) peak = p.nav;
    const dd = peak > 0 ? ((p.nav - peak) / peak) * 100 : 0;
    return { time: p.date, value: Number(dd.toFixed(2)) };
  });
}

/* ------------------------------------------------------------------ */
/*  Status helpers                                                     */
/* ------------------------------------------------------------------ */

const statusConfig: Record<string, { color: string; label: string }> = {
  completed: { color: 'success', label: '已完成' },
  running: { color: 'processing', label: '运行中' },
  failed: { color: 'error', label: '失败' },
};

/* ------------------------------------------------------------------ */
/*  Trade table columns                                                */
/* ------------------------------------------------------------------ */

const tradeColumns = [
  { title: '日期', dataIndex: 'date', key: 'date', width: 120 },
  { title: '标的', dataIndex: 'symbol', key: 'symbol', width: 120 },
  {
    title: '方向', dataIndex: 'direction', key: 'direction', width: 80,
    render: (dir: string) => (
      <Tag color={dir === 'BUY' || dir === 'buy' ? 'red' : 'green'}>
        {dir === 'BUY' || dir === 'buy' ? '买入' : '卖出'}
      </Tag>
    ),
  },
  {
    title: '数量', dataIndex: 'quantity', key: 'quantity', width: 100, align: 'right' as const,
    render: (v: number) => v?.toLocaleString() ?? '-',
  },
  {
    title: '成交价', dataIndex: 'price', key: 'price', width: 100, align: 'right' as const,
    render: (v: number) => v != null ? `¥${v.toFixed(2)}` : '-',
  },
];

/* ------------------------------------------------------------------ */
/*  Component                                                          */
/* ------------------------------------------------------------------ */

const BacktestResult: React.FC = () => {
  const { id } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const { token } = theme.useToken();

  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [metrics, setMetrics] = useState<BacktestMetrics | null>(null);
  const [navCurve, setNavCurve] = useState<NavPoint[]>([]);
  const [trades, setTrades] = useState<TradeRecord[]>([]);
  const [status, setStatus] = useState<string>('pending');

  const equityContainerRef = useRef<HTMLDivElement>(null);
  const drawdownContainerRef = useRef<HTMLDivElement>(null);

  // Fetch result from API
  useEffect(() => {
    if (!id) return;
    let cancelled = false;

    const fetchResult = async () => {
      setLoading(true);
      setError(null);
      try {
        const res = await fetch(`${API_BASE}/backtest/${id}/result`);
        if (!res.ok) {
          const errData = await res.json().catch(() => ({ detail: '未知错误' }));
          throw new Error(errData.detail || '结果尚未就绪');
        }
        const data = await res.json();
        if (!cancelled) {
          setMetrics(data.metrics ?? null);
          setNavCurve(data.nav_curve ?? []);
          setTrades(data.trades ?? []);
          setStatus(data.status ?? 'completed');
        }
      } catch (e: unknown) {
        if (!cancelled) {
          setError(e instanceof Error ? e.message : '加载失败');
        }
      } finally {
        if (!cancelled) setLoading(false);
      }
    };

    fetchResult();
    return () => { cancelled = true; };
  }, [id]);

  // Equity curve chart
  useEffect(() => {
    if (!navCurve.length || !equityContainerRef.current) return;

    const chart = createChart(equityContainerRef.current, {
      layout: { background: { color: 'transparent' }, textColor: token.colorText },
      grid: {
        vertLines: { color: token.colorBorder },
        horzLines: { color: token.colorBorder },
      },
      width: equityContainerRef.current.clientWidth,
      height: 400,
      rightPriceScale: { borderColor: token.colorBorder },
      timeScale: { borderColor: token.colorBorder, timeVisible: false },
      crosshair: {
        horzLine: {
          color: token.colorTextSecondary ?? token.colorText,
          labelBackgroundColor: token.colorPrimary,
        },
      },
      handleScroll: false,
      handleScale: false,
    });

    const lineSeries = chart.addSeries(LineSeries, {
      color: token.colorPrimary,
      lineWidth: 2,
      crosshairMarkerVisible: true,
      crosshairMarkerRadius: 4,
      priceFormat: { type: 'custom' as const, formatter: (v: number) => v.toFixed(2) },
      lastValueVisible: true,
      priceLineVisible: false,
    });

    lineSeries.setData(navCurve.map(p => ({ time: p.date as Time, value: p.nav })));
    chart.timeScale().fitContent();

    const handleResize = () => {
      if (equityContainerRef.current) chart.applyOptions({ width: equityContainerRef.current.clientWidth });
    };
    window.addEventListener('resize', handleResize);
    return () => {
      window.removeEventListener('resize', handleResize);
      chart.remove();
    };
  }, [navCurve, token]);

  // Drawdown chart
  useEffect(() => {
    if (!navCurve.length || !drawdownContainerRef.current) return;

    const ddData = computeDrawdown(navCurve);
    const chart = createChart(drawdownContainerRef.current, {
      layout: { background: { color: 'transparent' }, textColor: token.colorText },
      grid: {
        vertLines: { color: token.colorBorder },
        horzLines: { color: token.colorBorder },
      },
      width: drawdownContainerRef.current.clientWidth,
      height: 250,
      rightPriceScale: { borderColor: token.colorBorder, autoScale: true },
      timeScale: { borderColor: token.colorBorder, timeVisible: false },
      handleScroll: false,
      handleScale: false,
    });

    const areaSeries = chart.addSeries(AreaSeries, {
      lineColor: token.colorWarning,
      topColor: `${token.colorWarning}40`,
      bottomColor: `${token.colorWarning}08`,
      lineWidth: 2,
      priceFormat: { type: 'custom' as const, formatter: (v: number) => `${v.toFixed(2)}%` },
      lastValueVisible: true,
      priceLineVisible: false,
    });

    areaSeries.setData(ddData.map(p => ({ time: p.time as Time, value: p.value })));
    chart.timeScale().fitContent();

    const handleResize = () => {
      if (drawdownContainerRef.current) chart.applyOptions({ width: drawdownContainerRef.current.clientWidth });
    };
    window.addEventListener('resize', handleResize);
    return () => {
      window.removeEventListener('resize', handleResize);
      chart.remove();
    };
  }, [navCurve, token]);

  if (loading) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
        <Spin size="large" tip="正在加载回测结果...">
          <div style={{ padding: 50 }} />
        </Spin>
      </div>
    );
  }

  if (error) {
    return (
      <Alert
        message="加载失败"
        description={error}
        type="error"
        showIcon
        action={<Button size="small" onClick={() => navigate('/backtest')}>返回回测列表</Button>}
      />
    );
  }

  if (!metrics) {
    return (
      <Empty description="暂无回测数据">
        <Button onClick={() => navigate('/backtest')}>返回回测列表</Button>
      </Empty>
    );
  }

  const statusInfo = statusConfig[status] ?? { color: 'default', label: status };
  const profitColor = token.colorSuccess;
  const lossColor = token.colorError;

  return (
    <div style={{ '--profit-color': profitColor, '--loss-color': lossColor } as React.CSSProperties}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 8 }}>
        <Button icon={<ArrowLeftOutlined />} shape="circle" size="small" onClick={() => navigate('/backtest')} />
        <Title level={4} style={{ margin: 0 }}>回测结果</Title>
      </div>

      <Descriptions size="small" bordered column={{ xs: 1, sm: 2, md: 4 }} style={{ marginBottom: 16 }}>
        <Descriptions.Item label="回测ID"><Text code>{id}</Text></Descriptions.Item>
        <Descriptions.Item label="状态"><Tag color={statusInfo.color}>{statusInfo.label}</Tag></Descriptions.Item>
      </Descriptions>

      <Title level={5}>绩效指标</Title>
      <Row gutter={[12, 12]} style={{ marginBottom: 24 }}>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="总收益率" value={(metrics.total_return * 100)} suffix="%" precision={2}
              valueStyle={{ color: metrics.total_return >= 0 ? profitColor : lossColor }} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="年化收益" value={(metrics.annual_return * 100)} suffix="%" precision={2}
              valueStyle={{ color: metrics.annual_return >= 0 ? profitColor : lossColor }} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="夏普比率" value={metrics.sharpe_ratio} precision={2} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="最大回撤" value={Math.abs(metrics.max_drawdown * 100)} suffix="%" precision={2}
              valueStyle={{ color: lossColor }} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="胜率" value={metrics.win_rate * 100} suffix="%" precision={1} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="盈亏比" value={metrics.profit_factor} precision={2} />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic title="总交易次数" value={metrics.total_trades} />
          </Card>
        </Col>
      </Row>

      <Title level={5}>净值曲线</Title>
      <Card style={{ marginBottom: 16 }} styles={{ body: { padding: 0 } }}>
        <div ref={equityContainerRef} style={{ width: '100%' }} />
      </Card>

      <Title level={5}>回撤曲线</Title>
      <Card style={{ marginBottom: 24 }} styles={{ body: { padding: 0 } }}>
        <div ref={drawdownContainerRef} style={{ width: '100%' }} />
      </Card>

      {trades.length > 0 && (
        <>
          <Divider titlePlacement="left" plain>交易明细</Divider>
          <Table
            dataSource={trades}
            columns={tradeColumns}
            rowKey={(_, idx) => `${idx}`}
            pagination={{ pageSize: 10, showSizeChanger: false }}
            size="small"
            bordered
            style={{ marginBottom: 24 }}
          />
        </>
      )}
    </div>
  );
};

export default BacktestResult;