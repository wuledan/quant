import React from 'react';
import { Card, Col, Row, Statistic, Typography, Badge, Space } from 'antd';
import {
  ExperimentOutlined,
  FundOutlined,
  AlertOutlined,
  CheckCircleOutlined,
} from '@ant-design/icons';
import { useAppStore } from '../stores/appStore';

const { Title } = Typography;

const statusColorMap: Record<string, 'success' | 'error' | 'warning' | 'default'> = {
  running: 'success',
  stopped: 'default',
  error: 'error',
};

const statusLabelMap: Record<string, string> = {
  running: '运行中',
  stopped: '已停止',
  error: '异常',
};

const Dashboard: React.FC = () => {
  const { systemStatus } = useAppStore();

  return (
    <div>
      <Title level={4} style={{ marginTop: 0 }}>
        仪表盘
      </Title>

      <Row gutter={[16, 16]}>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="系统状态"
              value={statusLabelMap[systemStatus.engine]}
              prefix={
                <Badge status={statusColorMap[systemStatus.engine] ?? 'default'} />
              }
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="活跃策略"
              value={systemStatus.activeStrategies}
              prefix={<ExperimentOutlined />}
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="运行时长"
              value={Math.floor(systemStatus.uptime / 3600)}
              suffix="小时"
              prefix={<FundOutlined />}
            />
          </Card>
        </Col>
        <Col xs={24} sm={12} lg={6}>
          <Card>
            <Statistic
              title="最后心跳"
              value={systemStatus.lastHeartbeat ?? 'N/A'}
              prefix={<CheckCircleOutlined />}
            />
          </Card>
        </Col>
      </Row>

      <Row gutter={[16, 16]} style={{ marginTop: 16 }}>
        <Col xs={24} lg={12}>
          <Card title="快速操作">
            <Space direction="vertical" style={{ width: '100%' }}>
              <Space>
                <ExperimentOutlined /> 新建策略
              </Space>
              <Space>
                <FundOutlined /> 运行回测
              </Space>
              <Space>
                <AlertOutlined /> 查看风控
              </Space>
            </Space>
          </Card>
        </Col>
        <Col xs={24} lg={12}>
          <Card title="系统提示">
            <Space direction="vertical" style={{ width: '100%' }}>
              <Space>
                <Badge status="processing" />
                系统运行正常
              </Space>
              <Space>
                <Badge status="default" />
                上次同步: 刚刚
              </Space>
            </Space>
          </Card>
        </Col>
      </Row>
    </div>
  );
};

export default Dashboard;
