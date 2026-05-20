import React from 'react';
import { Card, Tag, Typography, Space } from 'antd';
import {
  ExperimentOutlined,
  ClockCircleOutlined,
} from '@ant-design/icons';
import type { Strategy } from '../../api/strategy';

const { Text } = Typography;

const statusColors: Record<string, string> = {
  active: 'green',
  draft: 'default',
  archived: 'orange',
  error: 'red',
};

interface StrategyCardProps {
  strategy: Strategy;
  onClick?: () => void;
}

const StrategyCard: React.FC<StrategyCardProps> = ({ strategy, onClick }) => {
  return (
    <Card
      hoverable={!!onClick}
      onClick={onClick}
      style={{ borderRadius: 8, textAlign: 'left' }}
      styles={{ body: { padding: 16 } }}
    >
      <Space direction="vertical" size="small" style={{ width: '100%' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <Text strong style={{ fontSize: 16, color: 'var(--text-h)' }}>
            <ExperimentOutlined style={{ marginRight: 6 }} />
            {strategy.name}
          </Text>
          <Tag color={statusColors[strategy.status] || 'default'}>
            {strategy.status}
          </Tag>
        </div>
        <div>
          <Text type="secondary" style={{ fontSize: 13 }}>
            Type: {strategy.type}
          </Text>
        </div>
        <div>
          <Text type="secondary" style={{ fontSize: 12 }}>
            <ClockCircleOutlined style={{ marginRight: 4 }} />
            Updated: {new Date(strategy.updated_at).toLocaleString()}
          </Text>
        </div>
      </Space>
    </Card>
  );
};

export default StrategyCard;
