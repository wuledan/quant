import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { Card, Col, Row, Statistic, Typography, Badge, Space, Spin } from 'antd';
import {
  ExperimentOutlined,
  FundOutlined,
  AlertOutlined,
  CheckCircleOutlined,
  CloudSyncOutlined,
  DatabaseOutlined,
} from '@ant-design/icons';

const { Title, Text } = Typography;
const API_BASE = '/api/v1';

interface SchedulerStatus {
  running: boolean;
  symbols: string[];
  daily_cache_count: number;
  news_cache_count: number;
  news_last_update: string | null;
  daily_cache_times: Record<string, string>;
}

const Dashboard: React.FC = () => {
  const navigate = useNavigate();
  const [schedulerStatus, setSchedulerStatus] = useState<SchedulerStatus | null>(null);
  const [systemRunning, setSystemRunning] = useState(false);

  const fetchStatus = async () => {
    try {
      const res = await fetch(`${API_BASE}/data/scheduler_status`);
      if (res.ok) {
        const data = await res.json();
        setSchedulerStatus(data);
        setSystemRunning(data.running);
      }
    } catch {
      setSystemRunning(false);
    }
  };

  useEffect(() => {
    fetchStatus();
    const timer = setInterval(fetchStatus, 30000);
    return () => clearInterval(timer);
  }, []);

  return (
    <div>
      <Title level={4} style={{ marginTop: 0 }}>仪表盘</Title>

      <Row gutter={[16, 16]}>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="系统状态"
              value={systemRunning ? '运行中' : '已停止'}
              prefix={<Badge status={systemRunning ? 'success' : 'default'} />}
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="监控标的"
              value={schedulerStatus?.symbols?.length ?? 0}
              suffix="个"
              prefix={<DatabaseOutlined />}
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="行情缓存"
              value={schedulerStatus?.daily_cache_count ?? 0}
              suffix="个标的"
              prefix={<CloudSyncOutlined />}
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="新闻缓存"
              value={schedulerStatus?.news_cache_count ?? 0}
              suffix="条"
              prefix={<FundOutlined />}
            />
          </Card>
        </Col>
      </Row>

      <Row gutter={[16, 16]} style={{ marginTop: 16 }}>
        <Col xs={24} lg={12}>
          <Card title="快速操作">
            <Space direction="vertical" style={{ width: '100%' }}>
              <Space style={{ cursor: 'pointer', padding: '8px 0', width: '100%' }} onClick={() => navigate('/strategies/new')}>
                <ExperimentOutlined /> 新建策略
              </Space>
              <Space style={{ cursor: 'pointer', padding: '8px 0', width: '100%' }} onClick={() => navigate('/backtest')}>
                <FundOutlined /> 运行回测
              </Space>
              <Space style={{ cursor: 'pointer', padding: '8px 0', width: '100%' }} onClick={() => navigate('/market')}>
                <CloudSyncOutlined /> 行情数据
              </Space>
              <Space style={{ cursor: 'pointer', padding: '8px 0', width: '100%' }} onClick={() => navigate('/news')}>
                <AlertOutlined /> 新闻资讯
              </Space>
            </Space>
          </Card>
        </Col>
        <Col xs={24} lg={12}>
          <Card title="数据调度状态">
            {schedulerStatus ? (
              <Space direction="vertical" style={{ width: '100%' }}>
                <Space>
                  <Badge status={schedulerStatus.running ? 'processing' : 'default'} />
                  <Text>调度服务: {schedulerStatus.running ? '运行中' : '未启动'}</Text>
                </Space>
                <Space>
                  <Badge status={schedulerStatus.daily_cache_count > 0 ? 'success' : 'warning'} />
                  <Text>行情缓存: {schedulerStatus.daily_cache_count} 个标的</Text>
                </Space>
                <Space>
                  <Badge status={schedulerStatus.news_cache_count > 0 ? 'success' : 'warning'} />
                  <Text>新闻缓存: {schedulerStatus.news_cache_count} 条</Text>
                </Space>
                {schedulerStatus.news_last_update && (
                  <Space>
                    <CheckCircleOutlined />
                    <Text type="secondary">新闻更新: {new Date(schedulerStatus.news_last_update).toLocaleTimeString('zh-CN')}</Text>
                  </Space>
                )}
                <Text type="secondary" style={{ fontSize: 12 }}>
                  监控: {schedulerStatus.symbols?.join(', ')}
                </Text>
              </Space>
            ) : (
              <Spin />
            )}
          </Card>
        </Col>
      </Row>
    </div>
  );
};

export default Dashboard;