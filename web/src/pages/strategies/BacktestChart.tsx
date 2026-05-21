import React from 'react';
import { Card, Row, Col, Statistic } from 'antd';
import { ArrowUpOutlined, ArrowDownOutlined } from '@ant-design/icons';
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip,
  ResponsiveContainer, ReferenceLine,
} from 'recharts';
import type { BacktestResult } from '../../api/strategy';

interface BacktestChartProps {
  result: BacktestResult;
}

const formatNav = (value: number): string => {
  if (value >= 1e8) return `${(value / 1e8).toFixed(2)}亿`;
  if (value >= 1e4) return `${(value / 1e4).toFixed(2)}万`;
  return value.toFixed(2);
};

const BacktestChart: React.FC<BacktestChartProps> = ({ result }) => {
  const { total_return, annual_return, max_drawdown, sharpe_ratio, total_trades, nav_curve } = result;

  // Convert timestamp to readable date for chart
  const chartData = nav_curve.map((point) => ({
    ...point,
    date: new Date(point.timestamp * 1000).toLocaleDateString('zh-CN', { month: '2-digit', day: '2-digit' }),
  }));

  const isPositiveReturn = total_return >= 0;

  return (
    <div>
      {/* Key metrics cards */}
      <Row gutter={16} style={{ marginBottom: 24 }}>
        <Col span={6}>
          <Card size="small">
            <Statistic
              title="总收益"
              value={total_return * 100}
              precision={2}
              suffix="%"
              valueStyle={{ color: isPositiveReturn ? '#3f8600' : '#cf1322' }}
              prefix={isPositiveReturn ? <ArrowUpOutlined /> : <ArrowDownOutlined />}
            />
          </Card>
        </Col>
        <Col span={6}>
          <Card size="small">
            <Statistic
              title="年化收益"
              value={annual_return * 100}
              precision={2}
              suffix="%"
              valueStyle={{ color: annual_return >= 0 ? '#3f8600' : '#cf1322' }}
            />
          </Card>
        </Col>
        <Col span={6}>
          <Card size="small">
            <Statistic
              title="最大回撤"
              value={max_drawdown * 100}
              precision={2}
              suffix="%"
              valueStyle={{ color: '#cf1322' }}
            />
          </Card>
        </Col>
        <Col span={6}>
          <Card size="small">
            <Statistic
              title="夏普比率"
              value={sharpe_ratio}
              precision={2}
              valueStyle={{ color: sharpe_ratio >= 1 ? '#3f8600' : '#faad14' }}
            />
          </Card>
        </Col>
      </Row>

      <Row gutter={16} style={{ marginBottom: 24 }}>
        <Col span={6}>
          <Card size="small">
            <Statistic title="总交易次数" value={total_trades} />
          </Card>
        </Col>
      </Row>

      {/* NAV curve chart */}
      {chartData.length > 0 && (
        <Card title="净值曲线" size="small">
          <ResponsiveContainer width="100%" height={360}>
            <LineChart data={chartData} margin={{ top: 5, right: 30, left: 20, bottom: 5 }}>
              <CartesianGrid strokeDasharray="3 3" />
              <XAxis dataKey="date" tick={{ fontSize: 12 }} />
              <YAxis
                tick={{ fontSize: 12 }}
                tickFormatter={formatNav}
                domain={['auto', 'auto']}
              />
              <Tooltip
                formatter={(value: unknown) => [formatNav(Number(value ?? 0)), '净值']}
                labelFormatter={(label) => `日期: ${String(label ?? '')}`}
              />
              <ReferenceLine
                y={chartData[0]?.nav}
                stroke="#888"
                strokeDasharray="3 3"
                label={{ value: '初始净值', position: 'insideTopRight', fontSize: 12 }}
              />
              <Line
                type="monotone"
                dataKey="nav"
                stroke={isPositiveReturn ? '#52c41a' : '#ff4d4f'}
                strokeWidth={2}
                dot={false}
                activeDot={{ r: 4 }}
              />
            </LineChart>
          </ResponsiveContainer>
        </Card>
      )}
    </div>
  );
};

export default BacktestChart;
