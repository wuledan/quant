import { useEffect, useRef, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import {
  Card, Col, Row, Statistic, Table, Tag, Descriptions, Divider,
  Typography, Spin, Alert, Empty, Button, theme,
} from 'antd';
import { ArrowLeftOutlined } from '@ant-design/icons';
import { createChart } from 'lightweight-charts';
import type { IChartApi, ISeriesApi, Time } from 'lightweight-charts';

const { Title, Text } = Typography;

/* ------------------------------------------------------------------ */
/*  Mock data types                                                    */
/* ------------------------------------------------------------------ */

interface MockTrade {
  date: string;
  symbol: string;
  direction: 'buy' | 'sell';
  quantity: number;
  price: number;
  pnl: number;
  cumulativePnl: number;
}

interface MockBacktestResult {
  id: string;
  status: 'completed' | 'running' | 'failed';
  strategyName: string;
  startDate: string;
  endDate: string;
  runtime: string;
  totalReturn: number;
  annualizedReturn: number;
  sharpeRatio: number;
  maxDrawdown: number;
  winRate: number;
  profitLossRatio: number;
  totalTrades: number;
  totalTurnover: number;
  equityCurve: { time: string; value: number }[];
  drawdownCurve: { time: string; value: number }[];
  trades: MockTrade[];
}

/* ------------------------------------------------------------------ */
/*  Mock data generation                                               */
/* ------------------------------------------------------------------ */

function generateMockEquityCurve(): { time: string; value: number }[] {
  const points: { time: string; value: number }[] = [];
  const start = new Date('2024-06-03');
  const end = new Date('2025-05-16');
  let nav = 1.0;
  const totalDays = Math.floor((end.getTime() - start.getTime()) / (1000 * 60 * 60 * 24));
  for (let i = 0; i <= totalDays; i++) {
    const d = new Date(start);
    d.setDate(d.getDate() + i);
    // Skip weekends
    if (d.getDay() === 0 || d.getDay() === 6) continue;
    // Skip a few random holidays (simplified)
    const month = d.getMonth() + 1;
    const day = d.getDate();
    if ((month === 10 && day >= 1 && day <= 7)) continue;
    if ((month === 2 && day >= 10 && day <= 17)) continue;
    // Random walk with drift
    const drift = 0.0004;
    const noise = (Math.random() - 0.48) * 0.012;
    nav = nav * (1 + drift + noise);
    if (nav < 0.92) nav = 0.92; // floor
    const y = d.getFullYear();
    const m = String(d.getMonth() + 1).padStart(2, '0');
    const dd = String(d.getDate()).padStart(2, '0');
    points.push({ time: `${y}-${m}-${dd}`, value: Number(nav.toFixed(4)) });
  }
  return points;
}

function computeDrawdown(
  equity: { time: string; value: number }[],
): { time: string; value: number }[] {
  let peak = -Infinity;
  return equity.map((p) => {
    if (p.value > peak) peak = p.value;
    const dd = peak > 0 ? (p.value - peak) / peak : 0;
    return { time: p.time, value: Number((dd * 100).toFixed(2)) };
  });
}

const MOCK_SYMBOLS = [
  '600519', '000858', '601318', '000333',
  '600036', '002415', '300750', '000002',
];
const MOCK_NAMES: Record<string, string> = {
  '600519': '贵州茅台',
  '000858': '五粮液',
  '601318': '中国平安',
  '000333': '美的集团',
  '600036': '招商银行',
  '002415': '海康威视',
  '300750': '宁德时代',
  '000002': '万科A',
};

function generateMockTrades(): MockTrade[] {
  const trades: MockTrade[] = [];
  let cumPnl = 0;
  const start = new Date('2024-06-10');
  const count = 3 + Math.floor(Math.random() * 8);
  for (let i = 0; i < count; i++) {
    const d = new Date(start);
    d.setDate(d.getDate() + 7 + i * (12 + Math.floor(Math.random() * 15)));
    if (d.getDay() === 0) d.setDate(d.getDate() + 1);
    if (d.getDay() === 6) d.setDate(d.getDate() + 2);
    const sym = MOCK_SYMBOLS[Math.floor(Math.random() * MOCK_SYMBOLS.length)];
    const dir: 'buy' | 'sell' = Math.random() > 0.5 ? 'buy' : 'sell';
    const qty = (1 + Math.floor(Math.random() * 10)) * 100;
    const price = 10 + Math.random() * 150;
    const pnlRaw = (Math.random() - 0.42) * (dir === 'buy' ? 1 : -1) * qty * price * 0.005;
    const pnl = Number(pnlRaw.toFixed(2));
    cumPnl += pnl;
    const y = d.getFullYear();
    const m = String(d.getMonth() + 1).padStart(2, '0');
    const dd = String(d.getDate()).padStart(2, '0');
    trades.push({
      date: `${y}-${m}-${dd}`,
      symbol: `${sym} ${MOCK_NAMES[sym] ?? sym}`,
      direction: dir,
      quantity: qty,
      price: Number(price.toFixed(2)),
      pnl,
      cumulativePnl: Number(cumPnl.toFixed(2)),
    });
  }
  return trades;
}

function generateMockResult(id: string): MockBacktestResult {
  const equity = generateMockEquityCurve();
  const drawdown = computeDrawdown(equity);
  const trades = generateMockTrades();
  const firstNav = equity[0]?.value ?? 1;
  const lastNav = equity[equity.length - 1]?.value ?? 1;
  const totalReturn = (lastNav / firstNav - 1) * 100;
  const tradingDays = equity.length;
  const years = tradingDays / 252;
  const anRet = years > 0 ? ((1 + totalReturn / 100) ** (1 / years) - 1) * 100 : 0;
  // Simulate sharpe ~1.8
  const dailyReturns = equity.slice(1).map((p, i) => (p.value / equity[i].value) - 1);
  const avgRet = dailyReturns.reduce((a, b) => a + b, 0) / dailyReturns.length;
  const stdRet = Math.sqrt(
    dailyReturns.reduce((a, b) => a + (b - avgRet) ** 2, 0) / dailyReturns.length,
  );
  const sharpe = stdRet > 0 ? (avgRet / stdRet) * Math.sqrt(252) : 0;
  const maxDd = Math.min(...drawdown.map((d) => d.value));
  const winTrades = trades.filter((t) => t.pnl > 0).length;
  const winRate = trades.length > 0 ? (winTrades / trades.length) * 100 : 0;
  const avgWin = trades.filter((t) => t.pnl > 0).reduce((a, b) => a + b.pnl, 0) / Math.max(winTrades, 1);
  const lossTrades = trades.filter((t) => t.pnl <= 0).length;
  const avgLoss = Math.abs(
    trades.filter((t) => t.pnl <= 0).reduce((a, b) => a + b.pnl, 0) / Math.max(lossTrades, 1),
  );
  const plRatio = avgLoss > 0 ? avgWin / avgLoss : avgWin;
  const turnover = trades.reduce((a, b) => a + b.quantity * b.price, 0);
  return {
    id,
    status: 'completed',
    strategyName: 'Alpha因子选股策略',
    startDate: equity[0]?.time ?? '2024-06-03',
    endDate: equity[equity.length - 1]?.time ?? '2025-05-16',
    runtime: '3分28秒',
    totalReturn: Number(totalReturn.toFixed(2)),
    annualizedReturn: Number(anRet.toFixed(2)),
    sharpeRatio: Number(sharpe.toFixed(2)),
    maxDrawdown: Number(maxDd.toFixed(2)),
    winRate: Number(winRate.toFixed(1)),
    profitLossRatio: Number(plRatio.toFixed(2)),
    totalTrades: trades.length,
    totalTurnover: Number(turnover.toFixed(0)),
    equityCurve: equity,
    drawdownCurve: drawdown,
    trades,
  };
}

/* ------------------------------------------------------------------ */
/*  Status helpers                                                     */
/* ------------------------------------------------------------------ */

const statusConfig: Record<
  string,
  { color: string; label: string }
> = {
  completed: { color: 'success', label: '已完成' },
  running: { color: 'processing', label: '运行中' },
  failed: { color: 'error', label: '失败' },
};

/* ------------------------------------------------------------------ */
/*  Trade table columns                                                */
/* ------------------------------------------------------------------ */

const tradeColumns = [
  {
    title: '日期',
    dataIndex: 'date',
    key: 'date',
    width: 120,
  },
  {
    title: '标的',
    dataIndex: 'symbol',
    key: 'symbol',
    width: 180,
  },
  {
    title: '方向',
    dataIndex: 'direction',
    key: 'direction',
    width: 80,
    render: (dir: string) => (
      <Tag color={dir === 'buy' ? 'red' : 'green'}>
        {dir === 'buy' ? '买入' : '卖出'}
      </Tag>
    ),
  },
  {
    title: '数量',
    dataIndex: 'quantity',
    key: 'quantity',
    width: 100,
    align: 'right' as const,
    render: (v: number) => v.toLocaleString(),
  },
  {
    title: '成交价',
    dataIndex: 'price',
    key: 'price',
    width: 100,
    align: 'right' as const,
    render: (v: number) => `¥${v.toFixed(2)}`,
  },
  {
    title: '盈亏',
    dataIndex: 'pnl',
    key: 'pnl',
    width: 120,
    align: 'right' as const,
    render: (v: number) => (
      <span style={{ color: v >= 0 ? 'var(--profit-color)' : 'var(--loss-color)' }}>
        {v >= 0 ? '+' : ''}¥{v.toFixed(2)}
      </span>
    ),
  },
  {
    title: '累计盈亏',
    dataIndex: 'cumulativePnl',
    key: 'cumulativePnl',
    width: 130,
    align: 'right' as const,
    render: (v: number) => (
      <span style={{ color: v >= 0 ? 'var(--profit-color)' : 'var(--loss-color)' }}>
        {v >= 0 ? '+' : ''}¥{v.toFixed(2)}
      </span>
    ),
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
  const [data, setData] = useState<MockBacktestResult | null>(null);

  const equityContainerRef = useRef<HTMLDivElement>(null);
  const drawdownContainerRef = useRef<HTMLDivElement>(null);
  const equityChartRef = useRef<IChartApi | null>(null);
  const drawdownChartRef = useRef<IChartApi | null>(null);

  // -- Simulate data fetching -------------------------------------------
  useEffect(() => {
    setLoading(true);
    setError(null);
    const timer = setTimeout(() => {
      try {
        const result = generateMockResult(id ?? 'unknown');
        setData(result);
      } catch {
        setError('加载回测结果失败，请稍后重试。');
      } finally {
        setLoading(false);
      }
    }, 600);
    return () => clearTimeout(timer);
  }, [id]);

  // -- Equity curve chart -----------------------------------------------
  useEffect(() => {
    if (!data || !equityContainerRef.current) return;

    const chart = createChart(equityContainerRef.current, {
      layout: {
        background: { color: 'transparent' },
        textColor: token.colorText,
      },
      grid: {
        vertLines: { color: token.colorBorder },
        horzLines: { color: token.colorBorder },
      },
      width: equityContainerRef.current.clientWidth,
      height: 400,
      rightPriceScale: {
        borderColor: token.colorBorder,
      },
      timeScale: {
        borderColor: token.colorBorder,
        timeVisible: false,
      },
      crosshair: {
        horzLine: {
          color: token.colorTextSecondary ?? token.colorText,
          labelBackgroundColor: token.colorPrimary,
        },
      },
      handleScroll: false,
      handleScale: false,
    });

    equityChartRef.current = chart;

    const lineSeries = chart.addLineSeries({
      color: token.colorPrimary,
      lineWidth: 2,
      crosshairMarkerVisible: true,
      crosshairMarkerRadius: 4,
      priceFormat: {
        type: 'custom' as const,
        formatter: (v: number) => v.toFixed(4),
      },
      lastValueVisible: true,
      priceLineVisible: false,
    });

    const chartData = data.equityCurve.map((p) => ({
      time: p.time as Time,
      value: p.value,
    }));
    lineSeries.setData(chartData);
    chart.timeScale().fitContent();

    const handleResize = () => {
      if (equityContainerRef.current) {
        chart.applyOptions({ width: equityContainerRef.current.clientWidth });
      }
    };
    window.addEventListener('resize', handleResize);

    return () => {
      window.removeEventListener('resize', handleResize);
      chart.remove();
      equityChartRef.current = null;
    };
  }, [data, token]);

  // -- Drawdown chart ---------------------------------------------------
  useEffect(() => {
    if (!data || !drawdownContainerRef.current) return;

    const chart = createChart(drawdownContainerRef.current, {
      layout: {
        background: { color: 'transparent' },
        textColor: token.colorText,
      },
      grid: {
        vertLines: { color: token.colorBorder },
        horzLines: { color: token.colorBorder },
      },
      width: drawdownContainerRef.current.clientWidth,
      height: 250,
      rightPriceScale: {
        borderColor: token.colorBorder,
        autoScale: true,
        invertScale: true,
      },
      timeScale: {
        borderColor: token.colorBorder,
        timeVisible: false,
      },
      handleScroll: false,
      handleScale: false,
    });

    drawdownChartRef.current = chart;

    const areaSeries = chart.addAreaSeries({
      lineColor: token.colorWarning,
      topColor: `${token.colorWarning}40`,
      bottomColor: `${token.colorWarning}08`,
      lineWidth: 2,
      priceFormat: {
        type: 'custom' as const,
        formatter: (v: number) => `${v.toFixed(2)}%`,
      },
      lastValueVisible: true,
      priceLineVisible: false,
    });

    const chartData = data.drawdownCurve.map((p) => ({
      time: p.time as Time,
      value: p.value,
    }));
    areaSeries.setData(chartData);
    chart.timeScale().fitContent();

    const handleResize = () => {
      if (drawdownContainerRef.current) {
        chart.applyOptions({ width: drawdownContainerRef.current.clientWidth });
      }
    };
    window.addEventListener('resize', handleResize);

    return () => {
      window.removeEventListener('resize', handleResize);
      chart.remove();
      drawdownChartRef.current = null;
    };
  }, [data, token]);

  // -- Loading state ----------------------------------------------------
  if (loading) {
    return (
      <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', minHeight: 400 }}>
        <Spin size="large" tip="正在加载回测结果...">
          <div style={{ padding: 50 }} />
        </Spin>
      </div>
    );
  }

  // -- Error state ------------------------------------------------------
  if (error) {
    return (
      <Alert
        message="加载失败"
        description={error}
        type="error"
        showIcon
        action={
          <Button size="small" onClick={() => navigate('/backtest')}>
            返回回测列表
          </Button>
        }
      />
    );
  }

  // -- Empty state ------------------------------------------------------
  if (!data) {
    return (
      <Empty description="暂无回测数据">
        <Button onClick={() => navigate('/backtest')}>返回回测列表</Button>
      </Empty>
    );
  }

  /* ---- Normal render ------------------------------------------------- */

  const statusInfo = statusConfig[data.status] ?? { color: 'default', label: data.status };

  const profitColor = token.colorSuccess;
  const lossColor = token.colorError;

  const cssVars = {
    '--profit-color': profitColor,
    '--loss-color': lossColor,
  } as React.CSSProperties;

  return (
    <div style={cssVars}>
      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, marginBottom: 8 }}>
        <Button
          icon={<ArrowLeftOutlined />}
          shape="circle"
          size="small"
          onClick={() => navigate('/backtest')}
        />
        <Title level={4} style={{ margin: 0 }}>
          回测结果
        </Title>
      </div>

      <Descriptions
        size="small"
        bordered
        column={{ xs: 1, sm: 2, md: 4 }}
        style={{ marginBottom: 16 }}
      >
        <Descriptions.Item label="回测ID">
          <Text code>{data.id}</Text>
        </Descriptions.Item>
        <Descriptions.Item label="策略名称">{data.strategyName}</Descriptions.Item>
        <Descriptions.Item label="状态">
          <Tag color={statusInfo.color}>{statusInfo.label}</Tag>
        </Descriptions.Item>
        <Descriptions.Item label="运行时长">{data.runtime}</Descriptions.Item>
        <Descriptions.Item label="开始日期">{data.startDate}</Descriptions.Item>
        <Descriptions.Item label="结束日期">{data.endDate}</Descriptions.Item>
      </Descriptions>

      {/* Performance Metrics */}
      <Title level={5}>绩效指标</Title>
      <Row gutter={[12, 12]} style={{ marginBottom: 24 }}>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="总收益率"
              value={data.totalReturn}
              suffix="%"
              precision={2}
              valueStyle={{ color: data.totalReturn >= 0 ? profitColor : lossColor }}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="年化收益"
              value={data.annualizedReturn}
              suffix="%"
              precision={2}
              valueStyle={{ color: data.annualizedReturn >= 0 ? profitColor : lossColor }}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="夏普比率"
              value={data.sharpeRatio}
              precision={2}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="最大回撤"
              value={Math.abs(data.maxDrawdown)}
              suffix="%"
              precision={2}
              valueStyle={{ color: lossColor }}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="胜率"
              value={data.winRate}
              suffix="%"
              precision={1}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="盈亏比"
              value={data.profitLossRatio}
              precision={2}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="总交易次数"
              value={data.totalTrades}
            />
          </Card>
        </Col>
        <Col xs={12} sm={8} md={6} lg={3}>
          <Card size="small" styles={{ body: { padding: '12px 16px' } }}>
            <Statistic
              title="总成交额"
              value={data.totalTurnover}
              precision={0}
              suffix="¥"
            />
          </Card>
        </Col>
      </Row>

      {/* Equity Curve */}
      <Title level={5}>权益曲线</Title>
      <Card
        style={{ marginBottom: 16 }}
        styles={{ body: { padding: 0 } }}
      >
        <div ref={equityContainerRef} style={{ width: '100%' }} />
      </Card>

      {/* Drawdown */}
      <Title level={5}>回撤曲线</Title>
      <Card
        style={{ marginBottom: 24 }}
        styles={{ body: { padding: 0 } }}
      >
        <div ref={drawdownContainerRef} style={{ width: '100%' }} />
      </Card>

      {/* Trade List */}
      <Divider orientation="left" plain>
        交易明细
      </Divider>
      <Table
        dataSource={data.trades}
        columns={tradeColumns}
        rowKey={(record, idx) => `${record.date}-${record.symbol}-${idx}`}
        pagination={{ pageSize: 10, showSizeChanger: false }}
        size="small"
        bordered
        style={{ marginBottom: 24 }}
      />
    </div>
  );
};

export default BacktestResult;
